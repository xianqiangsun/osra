/******************************************************************************
 OSRA: Optical Structure Recognition Application

 This is a U.S. Government work (2007-2011) and is therefore not subject to
 copyright. However, portions of this work were obtained from a GPL or
 GPL-compatible source.
 Created by Igor Filippov, 2007-2011 (igorf@helix.nih.gov)

 This program is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation; either version 2 of the License, or (at your option) any later
 version.

 This program is distributed in the hope that it will be useful, but WITHOUT ANY
 WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 PARTICULAR PURPOSE.  See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along with
 this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
 St, Fifth Floor, Boston, MA 02110-1301, USA
 *****************************************************************************/

#include <stdio.h> // fclose
#include <stdlib.h> // malloc(), free()
#include <math.h> // fabs(double)
#include <float.h> // FLT_MAX
#include <limits.h> // INT_MAX

#include <list> // sdt::list
#include <vector> // std::vector
#include <algorithm> // std::sort, std::min(double, double), std::max(double, double)
#include <iostream> // std::ostream, std::cout
#include <fstream> // std::ofstream, std::ifstream
#include <sstream> // std:ostringstream

#include <Magick++.h>

extern "C" {
#include <potracelib.h>
#include <pgm2asc.h>
}

#include <openbabel/oberror.h>

#include "osra.h"
#include "osra_grayscale.h"
#include "osra_segment.h"
#include "osra_lib.h"
#include "osra_ocr.h"
#include "osra_openbabel.h"
#include "osra_anisotropic.h"
#include "unpaper.h"
#include "config.h" // DATA_DIR

using namespace std;
using namespace Magick;

#define PI 3.14159265358979323846

#define BM_WORDSIZE ((int)sizeof(potrace_word))
#define BM_WORDBITS (8*BM_WORDSIZE)
#define BM_HIBIT (((potrace_word)1)<<(BM_WORDBITS-1))
#define bm_scanline(bm, y) ((bm)->map + (y)*(bm)->dy)
#define bm_index(bm, x, y) (&bm_scanline(bm, y)[(x)/BM_WORDBITS])
#define bm_mask(x) (BM_HIBIT >> ((x) & (BM_WORDBITS-1)))
#define bm_range(x, a) ((int)(x) >= 0 && (int)(x) < (a))
#define bm_safe(bm, x, y) (bm_range(x, (bm)->w) && bm_range(y, (bm)->h))
#define BM_USET(bm, x, y) (*bm_index(bm, x, y) |= bm_mask(x))
#define BM_UCLR(bm, x, y) (*bm_index(bm, x, y) &= ~bm_mask(x))
#define BM_UPUT(bm, x, y, b) ((b) ? BM_USET(bm, x, y) : BM_UCLR(bm, x, y))
#define BM_PUT(bm, x, y, b) (bm_safe(bm, x, y) ? BM_UPUT(bm, x, y, b) : 0)

// struct: letters_s
// character found as part of atomic label
struct letters_s
{
  // double: x,y,r
  // coordinates of the center and radius of the circumscribing circle
  double x, y, r;
  // char: a
  // character
  char a;
  // bool: free
  // whether or not it was already assign to an existing atomic label
  bool free;
};
// typedef: letters_t
// defines letters_t type based on letters_s struct
typedef struct letters_s letters_t;

// struct: label_s
// atomic label
struct label_s
{
  // doubles: x1,y1, x2, y2, r1, r2
  // central coordinates and circumradii for the first and last characters
  double x1, y1, r1, x2, y2, r2;
  // string: a
  // atomic label string
  string a;
  // array: n
  // vector of character indices comprising the atomic label
  vector<int> n;
};
// typedef: label_t
// defines label_t type based on label_s struct
typedef struct label_s label_t;

//struct: lbond_s
//pairs of characters used for constucting atomic labels in <osra.cpp::assemble_labels()>
struct lbond_s
{
  //int: a,b
  // indices of first and second character in a pair
  int a, b;
  //double: x
  // x-coordinate of the first character
  double x;
  //bool: exists
  //pair of characters is available
  bool exists;
};
//typedef: lbond_t
//defines lbond_t type based on lbond_s struct
typedef struct lbond_s lbond_t;

//struct: dash_s
// used to identify dashed bonds in <osra.cpp::find_dashed_bonds()> and small bonds in <osra.cpp::find_small_bonds()>
struct dash_s
{
  //double: x,y
  //coordinates
  double x, y;
  //bool: free
  // is this dash available for a perspective dashed bond?
  bool free;
  //pointer: curve
  // pointer to the curve found by Potrace
  const potrace_path_t *curve;
  //int: area
  // area occupied by the dash
  int area;
};
//typedef: dash_t
//defines dash_t type based on dash_s struct
typedef struct dash_s dash_t;

//struct: fragment_s
// used by <osra.cpp::populate_fragments()> to split chemical structure into unconnected molecules.
struct fragment_s
{
  //int: x1,y1,x2,y2
  //top left and bottom right coordinates of the fragment
  int x1, y1, x2, y2;
  //array: atom
  //vector of atom indices for atoms in a molecule of this fragment
  vector<int> atom;
};
//typedef: fragment_t
//defines fragment_t type based on fragment_s struct
typedef struct fragment_s fragment_t;

/* return new un-initialized bitmap. NULL with errno on error */
static potrace_bitmap_t *bm_new(int w, int h)
{
  potrace_bitmap_t *bm;
  int dy = (w + BM_WORDBITS - 1) / BM_WORDBITS;

  bm = (potrace_bitmap_t *) malloc(sizeof(potrace_bitmap_t));
  if (!bm)
    {
      return NULL;
    }
  bm->w = w;
  bm->h = h;
  bm->dy = dy;
  bm->map = (potrace_word *) malloc(dy * h * BM_WORDSIZE);
  if (!bm->map)
    {
      free(bm);
      return NULL;
    }
  return bm;
}

double distance(double x1, double y1, double x2, double y2)
{
  return (sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2)));
}

double atom_distance(const vector<atom_t> &atom, int a, int b)
{
  return (distance(atom[a].x, atom[a].y, atom[b].x, atom[b].y));
}

double angle4(double x1, double y1, double x2, double y2, double x3, double y3, double x4, double y4)
{
  double p, l1, l2, cos;

  p = (x1 - x2) * (x3 - x4) + (y1 - y2) * (y3 - y4);
  l1 = distance(x1, y1, x2, y2);
  l2 = distance(x4, y4, x3, y3);
  cos = p / (l1 * l2);
  return (cos);
}

void remove_disconnected_atoms(vector<atom_t> &atom, vector<bond_t> &bond, int n_atom, int n_bond)
{
  for (int i = 0; i < n_atom; i++)
    {
      if (atom[i].exists)
        {
          atom[i].exists = false;
          for (int j = 0; j < n_bond; j++)
            {
              if ((bond[j].exists) && (i == bond[j].a || i == bond[j].b))
                {
                  atom[i].exists = true;
                }
            }
        }
    }
}

void remove_zero_bonds(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom)
{
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      {
        for (int j = 0; j < n_bond; j++)
          if ((bond[j].exists) && (j != i) && ((bond[i].a == bond[j].a && bond[i].b == bond[j].b) || (bond[i].a
                                               == bond[j].b && bond[i].b == bond[j].a)))
            bond[j].exists = false;
        if (bond[i].a == bond[i].b)
          bond[i].exists = false;
        if (!atom[bond[i].a].exists || !atom[bond[i].b].exists)
          bond[i].exists = false;
      }
}

void collapse_doubleup_bonds(vector<bond_t> &bond, int n_bond)
{
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      for (int j = 0; j < n_bond; j++)
        if ((bond[j].exists) && (j != i) && ((bond[i].a == bond[j].a && bond[i].b == bond[j].b) || (bond[i].a
                                             == bond[j].b && bond[i].b == bond[j].a)))
          {
            bond[j].exists = false;
            bond[i].type++;
          }
}

int get_pixel(const Image &image, const ColorGray &bg, unsigned int x, unsigned int y, double THRESHOLD)
{
  if ((x < image.columns()) && (y < image.rows()))
    {
      ColorGray c = image.pixelColor(x, y);
      if (fabs(c.shade() - bg.shade()) > THRESHOLD)
        return (1);
    }
  return (0);
}

void delete_curve(vector<atom_t> &atom, vector<bond_t> &bond, int n_atom, int n_bond,
                  const potrace_path_t * const curve)
{
  for (int i = 0; i < n_atom; i++)
    {
      if (atom[i].curve == curve)
        {
          atom[i].exists = false;
        }
    }
  for (int i = 0; i < n_bond; i++)
    {
      if (bond[i].curve == curve)
        {
          bond[i].exists = false;
        }
    }
}

void delete_curve_with_children(vector<atom_t> &atom, vector<bond_t> &bond, int n_atom, int n_bond,
                                const potrace_path_t * const p)
{
  delete_curve(atom, bond, n_atom, n_bond, p);
  potrace_path_t *child = p->childlist;
  while (child != NULL)
    {
      delete_curve(atom, bond, n_atom, n_bond, child);
      child = child->sibling;
    }
}

void delete_bonds_in_char(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, double left, double top,
                          double right, double bottom)
{
  for (int j = 0; j < n_bond; j++)
    if (bond[j].exists && atom[bond[j].a].x >= left && atom[bond[j].a].x <= right && atom[bond[j].a].y >= top
        && atom[bond[j].a].y <= bottom && atom[bond[j].b].x >= left && atom[bond[j].b].x <= right
        && atom[bond[j].b].y >= top && atom[bond[j].b].y <= bottom)
      bond[j].exists = false;
}

double angle_between_bonds(const vector<bond_t> &bond, int i, int j, const vector<atom_t> &atom)
{
  return (angle4(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[j].a].x,
                 atom[bond[j].a].y, atom[bond[j].b].x, atom[bond[j].b].y));
}

double bond_length(const vector<bond_t> &bond, int i, const vector<atom_t> &atom)
{
  return (distance(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x, atom[bond[i].b].y));
}

double distance_from_bond_y(double x0, double y0, double x1, double y1, double x, double y)
{
  double d1 = distance(x0, y0, x1, y1);
  double cos = (x1 - x0) / d1;
  double sin = -(y1 - y0) / d1;
  double h = -(x - x0) * sin - (y - y0) * cos;
  return (h);
}

double distance_between_bonds(const vector<bond_t> &bond, int i, int j, const vector<atom_t> &atom)
{
  /*
  double y1 = distance_from_bond_y(atom[bond[j].a].x, atom[bond[j].a].y, atom[bond[j].b].x, atom[bond[j].b].y,
  		atom[bond[i].a].x, atom[bond[i].a].y);
  double y2 = distance_from_bond_y(atom[bond[j].a].x, atom[bond[j].a].y, atom[bond[j].b].x, atom[bond[j].b].y,
  		atom[bond[i].b].x, atom[bond[i].b].y);
  if (fabs(y1 - y2) >= 4)
  	return (FLT_MAX);
  double r1 = max(fabs(y1), fabs(y2));
  */
  double y3 = distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x, atom[bond[i].b].y,
                                   atom[bond[j].a].x, atom[bond[j].a].y);
  double y4 = distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x, atom[bond[i].b].y,
                                   atom[bond[j].b].x, atom[bond[j].b].y);
  if (fabs(y3 - y4) >= 4)
    return (FLT_MAX);
  double r2 = max(fabs(y3), fabs(y4));
  return (r2);
}

double distance_from_bond_x_a(double x0, double y0, double x1, double y1, double x, double y)
{
  double d1 = distance(x0, y0, x1, y1);
  double cos = (x1 - x0) / d1;
  double sin = -(y1 - y0) / d1;
  double l = (x - x0) * cos - (y - y0) * sin;
  return (l);
}

double distance_from_bond_x_b(double x0, double y0, double x1, double y1, double x, double y)
{
  double d1 = distance(x0, y0, x1, y1);
  double cos = (x1 - x0) / d1;
  double sin = -(y1 - y0) / d1;
  double l = (x - x0) * cos - (y - y0) * sin;
  return (l - d1);
}

void bond_end_swap(vector<bond_t> &bond, int i)
{
  int t = bond[i].a;
  bond[i].a = bond[i].b;
  bond[i].b = t;
}

bool bonds_within_each_other(const vector<bond_t> &bond, int ii, int jj, const vector<atom_t> &atom)
{
  int i, j;
  bool res = false;

  if (bond_length(bond, ii, atom) > bond_length(bond, jj, atom))
    {
      i = ii;
      j = jj;
    }
  else
    {
      i = jj;
      j = ii;
    }

  double x1 = atom[bond[i].a].x;
  double x2 = atom[bond[i].b].x;
  double y1 = atom[bond[i].a].y;
  double y2 = atom[bond[i].b].y;
  double d1 = bond_length(bond, i, atom);
  double x3 = distance_from_bond_x_a(x1, y1, x2, y2, atom[bond[j].a].x, atom[bond[j].a].y);
  double x4 = distance_from_bond_x_a(x1, y1, x2, y2, atom[bond[j].b].x, atom[bond[j].b].y);

  if ((x3 + x4) / 2 > 0 && (x3 + x4) / 2 < d1)
    res = true;

  return (res);
}

double percentile75(const vector<bond_t> &bond, int n_bond, const vector<atom_t> &atom)
{
  vector<double> a;
  int n = 0;

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      {
        a.push_back(bond_length(bond, i, atom));
        n++;
      }
  if (n > 1)
    {
      std::sort(a.begin(), a.end());
      int pos = 3 * (n - 1) / 4;
      return (a[pos]);
    }
  else
    return (10.0);
}

bool alone(const vector<bond_t> &bond, int i, double avg)
{
  bool alone = false;
  const potrace_path_t * const p = bond[i].curve;

  if ((p->sign == int('+')) && (p->area < 2 * avg))
    alone = true;

  return (alone);
}

bool no_white_space(int ai, int bi, int aj, int bj, const vector<atom_t> &atom, const Image &image, double threshold,
                    const ColorGray &bgColor)
{
  vector<double> xx(4);
  double dx1 = atom[bi].x - atom[ai].x;
  double dy1 = atom[bi].y - atom[ai].y;
  double dx2 = atom[bj].x - atom[aj].x;
  double dy2 = atom[bj].y - atom[aj].y;
  double k1, k2;
  int s = 0, w = 0;
  int total_length = 0, white_length = 0;

  if (fabs(dx1) > fabs(dy1))
    {
      xx[0] = atom[ai].x;
      xx[1] = atom[bi].x;
      xx[2] = atom[aj].x;
      xx[3] = atom[bj].x;
      std::sort(xx.begin(), xx.end());
      k1 = dy1 / dx1;
      k2 = dy2 / dx2;
      int d = (dx1 > 0 ? 1 : -1);

      for (int x = int(atom[ai].x); x != int(atom[bi].x); x += d)
        if (x > xx[1] && x < xx[2])
          {
            double p1 = (x - atom[ai].x) * k1 + atom[ai].y;
            double p2 = (x - atom[aj].x) * k2 + atom[aj].y;
            if (fabs(p2 - p1) < 1)
              continue;
            int dp = (p2 > p1 ? 1 : -1);
            bool white = false;
            for (int y = int(p1) + dp; y != int(p2); y += dp)
              {
                s++;
                if (get_pixel(image, bgColor, x, y, threshold) == 0)
                  {
                    w++;
                    white = true;
                  }
              }
            total_length++;
            if (white)
              white_length++;
          }
    }
  else
    {
      xx[0] = atom[ai].y;
      xx[1] = atom[bi].y;
      xx[2] = atom[aj].y;
      xx[3] = atom[bj].y;
      std::sort(xx.begin(), xx.end());
      k1 = dx1 / dy1;
      k2 = dx2 / dy2;
      int d = (dy1 > 0 ? 1 : -1);

      for (int y = int(atom[ai].y); y != int(atom[bi].y); y += d)
        if (y > xx[1] && y < xx[2])
          {
            double p1 = (y - atom[ai].y) * k1 + atom[ai].x;
            double p2 = (y - atom[aj].y) * k2 + atom[aj].x;
            if (fabs(p2 - p1) < 1)
              continue;
            int dp = (p2 > p1 ? 1 : -1);
            bool white = false;
            for (int x = int(p1) + dp; x != int(p2); x += dp)
              {
                s++;
                if (get_pixel(image, bgColor, x, y, threshold) == 0)
                  {
                    w++;
                    white = true;
                  }
              }
            total_length++;
            if (white)
              white_length++;
          }
    }
  //if (s == 0) return(true);
  //if ((1. * w) / s > WHITE_SPACE_FRACTION) return(false);
  if (total_length == 0)
    return (true);
  if ((1. * white_length) / total_length > 0.5)
    return (false);
  else
    return (true);

}

double skeletize(vector<atom_t> &atom, vector<bond_t> &bond, int n_bond, const Image &image, double threshold,
                 const ColorGray &bgColor, double dist, double avg)
{
  double thickness = 0;
  vector<double> a;
  int n = 0;

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && !bond[i].Small)
      {
        double l1 = bond_length(bond, i, atom);
        for (int j = 0; j < n_bond; j++)
          if (i != j && bond[j].exists && bonds_within_each_other(bond, i, j, atom) && !bond[j].Small)
            {
              double tt = distance_between_bonds(bond, i, j, atom);
              double tang = angle_between_bonds(bond, i, j, atom);
              if ((fabs(tang) > D_T_TOLERANCE && no_white_space(bond[i].a, bond[i].b, bond[j].a, bond[j].b, atom,
                   image, threshold, bgColor) && tt < MAX_BOND_THICKNESS) || tt < dist)
                {
                  double l2 = bond_length(bond, j, atom);
                  a.push_back(tt);
                  n++;
                  if (l1 < l2)
                    {
                      bond[i].exists = false;
                      bond[j].type = 1;
                      if (bond[i].arom)
                        bond[j].arom = true;
                      if (l1 > avg / 2)
                        {
                          double ay = fabs(distance_from_bond_y(atom[bond[j].a].x, atom[bond[j].a].y,
                                                                atom[bond[j].b].x, atom[bond[j].b].y, atom[bond[i].a].x, atom[bond[i].a].y));
                          double axa = fabs(distance_from_bond_x_a(atom[bond[j].a].x, atom[bond[j].a].y,
                                            atom[bond[j].b].x, atom[bond[j].b].y, atom[bond[i].a].x, atom[bond[i].a].y));
                          double axb = fabs(distance_from_bond_x_b(atom[bond[j].a].x, atom[bond[j].a].y,
                                            atom[bond[j].b].x, atom[bond[j].b].y, atom[bond[i].a].x, atom[bond[i].a].y));

                          if (tang > 0 && ay > axa)
                            {
                              atom[bond[i].a].x = (atom[bond[i].a].x + atom[bond[j].a].x) / 2;
                              atom[bond[i].a].y = (atom[bond[i].a].y + atom[bond[j].a].y) / 2;
                              atom[bond[j].a].x = (atom[bond[i].a].x + atom[bond[j].a].x) / 2;
                              atom[bond[j].a].y = (atom[bond[i].a].y + atom[bond[j].a].y) / 2;
                            }
                          if (tang < 0 && ay > axb)
                            {
                              atom[bond[i].a].x = (atom[bond[i].a].x + atom[bond[j].b].x) / 2;
                              atom[bond[i].a].y = (atom[bond[i].a].y + atom[bond[j].b].y) / 2;
                              atom[bond[j].b].x = (atom[bond[i].a].x + atom[bond[j].b].x) / 2;
                              atom[bond[j].b].y = (atom[bond[i].a].y + atom[bond[j].b].y) / 2;
                            }
                          double by = fabs(distance_from_bond_y(atom[bond[j].a].x, atom[bond[j].a].y,
                                                                atom[bond[j].b].x, atom[bond[j].b].y, atom[bond[i].b].x, atom[bond[i].b].y));
                          double bxa = fabs(distance_from_bond_x_a(atom[bond[j].a].x, atom[bond[j].a].y,
                                            atom[bond[j].b].x, atom[bond[j].b].y, atom[bond[i].b].x, atom[bond[i].b].y));
                          double bxb = fabs(distance_from_bond_x_b(atom[bond[j].a].x, atom[bond[j].a].y,
                                            atom[bond[j].b].x, atom[bond[j].b].y, atom[bond[i].b].x, atom[bond[i].b].y));

                          if (tang > 0 && by > bxb)
                            {
                              atom[bond[i].b].x = (atom[bond[i].b].x + atom[bond[j].b].x) / 2;
                              atom[bond[i].b].y = (atom[bond[i].b].y + atom[bond[j].b].y) / 2;
                              atom[bond[j].b].x = (atom[bond[i].b].x + atom[bond[j].b].x) / 2;
                              atom[bond[j].b].y = (atom[bond[i].b].y + atom[bond[j].b].y) / 2;
                            }
                          if (tang < 0 && by > bxa)
                            {
                              atom[bond[i].b].x = (atom[bond[i].b].x + atom[bond[j].a].x) / 2;
                              atom[bond[i].b].y = (atom[bond[i].b].y + atom[bond[j].a].y) / 2;
                              atom[bond[j].a].x = (atom[bond[i].b].x + atom[bond[j].a].x) / 2;
                              atom[bond[j].a].y = (atom[bond[i].b].y + atom[bond[j].a].y) / 2;
                            }
                        }
                      break;
                    }
                  else
                    {
                      bond[j].exists = false;
                      bond[i].type = 1;
                      if (bond[j].arom)
                        bond[i].arom = true;
                      if (l2 > avg / 2)
                        {
                          double ay = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                                atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[j].a].x, atom[bond[j].a].y));
                          double axa = fabs(distance_from_bond_x_a(atom[bond[i].a].x, atom[bond[i].a].y,
                                            atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[j].a].x, atom[bond[j].a].y));
                          double axb = fabs(distance_from_bond_x_b(atom[bond[i].a].x, atom[bond[i].a].y,
                                            atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[j].a].x, atom[bond[j].a].y));

                          if (tang > 0 && ay > axa)
                            {
                              atom[bond[i].a].x = (atom[bond[i].a].x + atom[bond[j].a].x) / 2;
                              atom[bond[i].a].y = (atom[bond[i].a].y + atom[bond[j].a].y) / 2;
                              atom[bond[j].a].x = (atom[bond[i].a].x + atom[bond[j].a].x) / 2;
                              atom[bond[j].a].y = (atom[bond[i].a].y + atom[bond[j].a].y) / 2;
                            }
                          if (tang < 0 && ay > axb)
                            {
                              atom[bond[j].a].x = (atom[bond[j].a].x + atom[bond[i].b].x) / 2;
                              atom[bond[j].a].y = (atom[bond[j].a].y + atom[bond[i].b].y) / 2;
                              atom[bond[i].b].x = (atom[bond[j].a].x + atom[bond[i].b].x) / 2;
                              atom[bond[i].b].y = (atom[bond[j].a].y + atom[bond[i].b].y) / 2;
                            }
                          double by = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                                atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[j].b].x, atom[bond[j].b].y));
                          double bxa = fabs(distance_from_bond_x_a(atom[bond[i].a].x, atom[bond[i].a].y,
                                            atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[j].b].x, atom[bond[j].b].y));
                          double bxb = fabs(distance_from_bond_x_b(atom[bond[i].a].x, atom[bond[i].a].y,
                                            atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[j].b].x, atom[bond[j].b].y));

                          if (tang > 0 && by > bxb)
                            {
                              atom[bond[i].b].x = (atom[bond[i].b].x + atom[bond[j].b].x) / 2;
                              atom[bond[i].b].y = (atom[bond[i].b].y + atom[bond[j].b].y) / 2;
                              atom[bond[j].b].x = (atom[bond[i].b].x + atom[bond[j].b].x) / 2;
                              atom[bond[j].b].y = (atom[bond[i].b].y + atom[bond[j].b].y) / 2;
                            }
                          if (tang < 0 && by > bxa)
                            {
                              atom[bond[j].b].x = (atom[bond[j].b].x + atom[bond[i].a].x) / 2;
                              atom[bond[j].b].y = (atom[bond[j].b].y + atom[bond[i].a].y) / 2;
                              atom[bond[i].a].x = (atom[bond[j].b].x + atom[bond[i].a].x) / 2;
                              atom[bond[i].a].y = (atom[bond[j].b].y + atom[bond[i].a].y) / 2;
                            }
                        }
                    }
                }
            }
      }
  std::sort(a.begin(), a.end());
  if (n > 0)
    thickness = a[(n - 1) / 2];
  else
    thickness = dist;
  return (thickness);
}

double dist_double_bonds(const vector<atom_t> &atom, vector<bond_t> &bond, int n_bond, double avg)
{
  vector<double> a;
  int n = 0;
  double max_dist_double_bond = 0;

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      {
        double l1 = bond_length(bond, i, atom);
        bond[i].conjoined = false;
        for (int j = i + 1; j < n_bond; j++)
          if ((bond[j].exists) && (fabs(angle_between_bonds(bond, i, j, atom)) > D_T_TOLERANCE))
            {
              double l2 = bond_length(bond, j, atom);
              double dbb = distance_between_bonds(bond, i, j, atom);
              if (dbb < avg / 2 && l1 > avg / 3 && l2 > avg / 3 && bonds_within_each_other(bond, i, j, atom))
                {
                  a.push_back(dbb);
                  n++;
                }
            }
      }
  std::sort(a.begin(), a.end());
  //for (int i = 0; i < n; i++) cout << a[i] << endl;
  //cout << "-----------------" << endl;
  if (n > 0)
    max_dist_double_bond = a[3 * (n - 1) / 4];

  if (max_dist_double_bond < 1)
    max_dist_double_bond = avg / 3;
  else
    {
      max_dist_double_bond += 2;
      for (int i = 0; i < n; i++)
        if (a[i] - max_dist_double_bond < 1 && a[i] > max_dist_double_bond)
          max_dist_double_bond = a[i];
    }
  max_dist_double_bond += 0.001;
  return (max_dist_double_bond);
}

int double_triple_bonds(vector<atom_t> &atom, vector<bond_t> &bond, int n_bond, double avg, int &n_atom,
                        double max_dist_double_bond)
{
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      {
        double l1 = bond_length(bond, i, atom);
        for (int j = i + 1; j < n_bond; j++)
          if ((bond[j].exists) && (fabs(angle_between_bonds(bond, i, j, atom)) > D_T_TOLERANCE))
            {
              double l2 = bond_length(bond, j, atom);
              double dij = distance_between_bonds(bond, i, j, atom);
              if (dij <= max_dist_double_bond && bonds_within_each_other(bond, i, j, atom))
                {
                  // start triple bond search
                  for (int k = j + 1; k < n_bond; k++)
                    if ((bond[k].exists) && (fabs(angle_between_bonds(bond, k, j, atom)) > D_T_TOLERANCE))
                      {
                        double l3 = bond_length(bond, k, atom);
                        double djk = distance_between_bonds(bond, k, j, atom);
                        double dik = distance_between_bonds(bond, k, i, atom);
                        if (djk <= max_dist_double_bond && bonds_within_each_other(bond, k, j, atom))
                          {
                            if (dik > dij)
                              {
                                bond[k].exists = false;
                                if ((l3 > l2 / 2) || (l2 > avg && l2 > 1.5 * l3 && l3 > 0.5 * avg))
                                  {
                                    bond[j].type += bond[k].type;
                                    if (bond[j].curve == bond[k].curve)
                                      bond[j].conjoined = true;
                                  }
                                if (bond[k].arom)
                                  bond[j].arom = true;
                              }
                            else
                              {
                                bond[j].exists = false;
                                if ((l2 > l3 / 2) || (l3 > avg && l3 > 1.5 * l2 && l2 > 0.5 * avg))
                                  {
                                    bond[k].type += bond[j].type;
                                    if (bond[j].curve == bond[k].curve)
                                      bond[k].conjoined = true;
                                  }
                                if (bond[j].arom)
                                  bond[k].arom = true;
                                break;
                              }
                          }
                      }

                  if (!bond[j].exists)
                    continue;
                  // end triple bond search

                  int ii = i, jj = j;
                  double l11 = l1, l22 = l2;
                  bool extended_triple = false;
                  if (l1 > avg && l1 > 1.5 * l2 && l2 > 0.5 * avg)
                    extended_triple = true;
                  else if (l2 > avg && l2 > 1.5 * l1 && l1 > 0.5 * avg)
                    {
                      ii = j;
                      jj = i;
                      l11 = l2;
                      l22 = l1;
                      extended_triple = true;
                    }
                  if (extended_triple)
                    {
                      double aa = fabs(distance_from_bond_x_a(atom[bond[ii].a].x, atom[bond[ii].a].y,
                                                              atom[bond[ii].b].x, atom[bond[ii].b].y, atom[bond[jj].a].x, atom[bond[jj].a].y));
                      double ab = fabs(distance_from_bond_x_a(atom[bond[ii].a].x, atom[bond[ii].a].y,
                                                              atom[bond[ii].b].x, atom[bond[ii].b].y, atom[bond[jj].b].x, atom[bond[jj].b].y));
                      double ba = fabs(distance_from_bond_x_b(atom[bond[ii].a].x, atom[bond[ii].a].y,
                                                              atom[bond[ii].b].x, atom[bond[ii].b].y, atom[bond[jj].a].x, atom[bond[jj].a].y));
                      double bb = fabs(distance_from_bond_x_b(atom[bond[ii].a].x, atom[bond[ii].a].y,
                                                              atom[bond[ii].b].x, atom[bond[ii].b].y, atom[bond[jj].b].x, atom[bond[jj].b].y));
                      double da = min(aa, ab);
                      double db = min(ba, bb);
                      if (da > 0.5 * l22)
                        {
                          double x = atom[bond[ii].a].x + (atom[bond[ii].b].x - atom[bond[ii].a].x) * da / l11;
                          double y = atom[bond[ii].a].y + (atom[bond[ii].b].y - atom[bond[ii].a].y) * da / l11;
                          bond_t b1;
                          bond.push_back(b1);
                          bond[n_bond].a = bond[ii].a;
                          bond[n_bond].exists = true;
                          bond[n_bond].type = 1;
                          bond[n_bond].curve = bond[ii].curve;
                          bond[n_bond].hash = false;
                          bond[n_bond].wedge = false;
                          bond[n_bond].up = false;
                          bond[n_bond].down = false;
                          bond[n_bond].Small = false;
                          bond[n_bond].arom = false;
                          bond[n_bond].conjoined = false;
                          atom_t a1;
                          atom.push_back(a1);
                          atom[n_atom].x = x;
                          atom[n_atom].y = y;
                          atom[n_atom].label = " ";
                          atom[n_atom].exists = true;
                          atom[n_atom].curve = bond[ii].curve;
                          atom[n_atom].n = 0;
                          atom[n_atom].corner = false;
                          atom[n_atom].terminal = false;
                          atom[n_atom].charge = 0;
                          atom[n_atom].anum = 0;
                          bond[ii].a = n_atom;
                          n_atom++;
                          if (n_atom >= MAX_ATOMS)
                            n_atom--;
                          bond[n_bond].b = bond[ii].a;
                          n_bond++;
                          if (n_bond >= MAX_ATOMS)
                            n_bond--;
                        }
                      if (db > 0.5 * l22)
                        {
                          double x = atom[bond[ii].b].x + (atom[bond[ii].a].x - atom[bond[ii].b].x) * db / l11;
                          double y = atom[bond[ii].b].y + (atom[bond[ii].a].y - atom[bond[ii].b].y) * db / l11;
                          bond_t b1;
                          bond.push_back(b1);
                          bond[n_bond].a = bond[ii].b;
                          bond[n_bond].exists = true;
                          bond[n_bond].type = 1;
                          bond[n_bond].curve = bond[ii].curve;
                          bond[n_bond].hash = false;
                          bond[n_bond].wedge = false;
                          bond[n_bond].up = false;
                          bond[n_bond].down = false;
                          bond[n_bond].Small = false;
                          bond[n_bond].arom = false;
                          bond[n_bond].conjoined = false;
                          atom_t a1;
                          atom.push_back(a1);
                          atom[n_atom].x = x;
                          atom[n_atom].y = y;
                          atom[n_atom].label = " ";
                          atom[n_atom].exists = true;
                          atom[n_atom].curve = bond[ii].curve;
                          atom[n_atom].n = 0;
                          atom[n_atom].corner = false;
                          atom[n_atom].terminal = false;
                          atom[n_atom].charge = 0;
                          atom[n_atom].anum = 0;
                          bond[ii].b = n_atom;
                          n_atom++;
                          if (n_atom >= MAX_ATOMS)
                            n_atom--;
                          bond[n_bond].b = bond[ii].b;
                          n_bond++;
                          if (n_bond >= MAX_ATOMS)
                            n_bond--;
                        }
                      bond[jj].exists = false;
                      bond[ii].type += bond[jj].type;
                      if (bond[jj].arom)
                        bond[ii].arom = true;
                      if (bond[jj].curve == bond[ii].curve)
                        bond[ii].conjoined = true;
                      if (i == jj)
                        break;
                    }
                  else
                    {
                      if (l1 > l2)
                        {
                          bond[j].exists = false;
                          if (l2 > l1 / 2)
                            {
                              bond[i].type += bond[j].type;
                              if (bond[j].curve == bond[i].curve)
                                bond[i].conjoined = true;
                            }
                          if (bond[j].arom)
                            bond[i].arom = true;
                        }
                      else
                        {
                          bond[i].exists = false;
                          if (l1 > l2 / 2)
                            {
                              bond[j].type += bond[i].type;
                              if (bond[j].curve == bond[i].curve)
                                bond[j].conjoined = true;
                            }
                          if (bond[i].arom)
                            bond[j].arom = true;
                          break;
                        }
                    }
                }
            }
      }
  return (n_bond);
}

bool chlorine(const vector<bond_t> &bond, const vector<atom_t> &atom, int i, vector<letters_t> &letters, int n_letters,
              int max_font_height, int min_font_height)
{
  bool res = false;
  double x = (atom[bond[i].a].x + atom[bond[i].b].x) / 2;
  double y = (atom[bond[i].a].y + atom[bond[i].b].y) / 2;
  double r = bond_length(bond, i, atom) / 2;
  if ((bond_length(bond, i, atom) < max_font_height) && (bond_length(bond, i, atom) > min_font_height) && (fabs(
        atom[bond[i].a].x - atom[bond[i].b].x) < fabs(atom[bond[i].a].y - atom[bond[i].b].y)))
    {
      for (int j = 0; j < n_letters; j++)
        {
          if ((distance(x, y, letters[j].x, letters[j].y) < r + letters[j].r) && (fabs(y - letters[j].y) < min(r,
              letters[j].r)))
            {
              res = true;
            }
        }
    }

  return (res);
}

int remove_small_bonds(vector<bond_t> &bond, int n_bond, const vector<atom_t> &atom, vector<letters_t> &letters,
                       int n_letters, int max_font_height, int min_font_height, double avg)
{
  for (int i = 0; i < n_bond; i++)
    if ((bond[i].exists) && (bond[i].type == 1))
      {
        bool al = alone(bond, i, avg);
        if (bond_length(bond, i, atom) < V_DISPLACEMENT)
          {
            bond[i].exists = false;
          }
        else if ((al) && (chlorine(bond, atom, i, letters, n_letters, max_font_height, min_font_height)))
          {
            letters_t lt;
            letters.push_back(lt);
            letters[n_letters].a = 'l';
            letters[n_letters].x = (atom[bond[i].a].x + atom[bond[i].b].x) / 2;
            letters[n_letters].y = (atom[bond[i].a].y + atom[bond[i].b].y) / 2;
            letters[n_letters].r = bond_length(bond, i, atom) / 2;
            letters[n_letters].free = true;
            n_letters++;
            if (n_letters >= MAX_ATOMS)
              n_letters--;
            bond[i].exists = false;
          }
      }
  return (n_letters);
}

bool comp_lbonds(const lbond_t &left, const lbond_t &right)
{
  if (left.x < right.x)
    return (true);
  if (left.x > right.x)
    return (false);
  return (false);
}

bool comp_letters(const letters_t &left, const letters_t &right)
{
  if (left.x < right.x)
    return (true);
  if (left.x > right.x)
    return (false);
  return (false);
}

bool terminal_bond(int a, int b, const vector<bond_t> &bond, int n_bond)
{
  bool terminal = true;

  for (int l = 0; l < n_bond; l++)
    if (l != b && bond[l].exists && (bond[l].a == a || bond[l].b == a))
      terminal = false;

  return (terminal);
}

int assemble_labels(vector<letters_t> &letters, int n_letters, vector<label_t> &label)
{
  vector<lbond_t> lbond;
  int n_lbond = 0;
  int n_label = 0;

  std::sort(letters.begin(), letters.end(), comp_letters);

  for (int i = 0; i < n_letters; i++)
    {
      for (int j = i + 1; j < n_letters; j++)
        if ((distance(letters[i].x, letters[i].y, letters[j].x, letters[j].y) < 2 * max(letters[i].r, letters[j].r)
             && (((fabs(letters[i].y - letters[j].y) < min(letters[i].r, letters[j].r))) || ((fabs(letters[i].y
                 - letters[j].y) < (letters[i].r + letters[j].r)) && (((letters[i].y < letters[j].y)
                     && (isdigit(letters[j].a))) || ((letters[j].y < letters[i].y) && (isdigit(letters[i].a)))))))
            || (distance(letters[i].x, letters[i].y, letters[j].x, letters[j].y) < 1.5 * (letters[i].r
                + letters[j].r) && (letters[i].a == '-' || letters[i].a == '+' || letters[j].a == '-'
                                    || letters[j].a == '+')))
          {
            lbond_t lb;
            lbond.push_back(lb);
            lbond[n_lbond].a = i;
            lbond[n_lbond].b = j;
            lbond[n_lbond].x = letters[i].x;

            letters[i].free = false;
            letters[j].free = false;
            lbond[n_lbond].exists = true;
            n_lbond++;
            if (n_lbond >= MAX_ATOMS)
              n_lbond--;
            break;
          }
    }

  std::sort(lbond.begin(), lbond.end(), comp_lbonds);

  for (int i = 0; i < n_lbond; i++)
    if (lbond[i].exists)
      {
        bool found_left = false;
        label_t lb;
        label.push_back(lb);
        label[n_label].x1 = FLT_MAX;
        label[n_label].y1 = FLT_MAX;
        label[n_label].r1 = 0;
        label[n_label].x2 = FLT_MAX;
        label[n_label].y2 = FLT_MAX;
        label[n_label].r2 = 0;
        label[n_label].a = letters[lbond[i].a].a;
        label[n_label].a += letters[lbond[i].b].a;
        label[n_label].n.push_back(lbond[i].a);
        label[n_label].n.push_back(lbond[i].b);
        if (!isdigit(letters[lbond[i].a].a) && letters[lbond[i].a].a != '-' && letters[lbond[i].a].a != '+'
            && !found_left)
          {
            label[n_label].x1 = letters[lbond[i].a].x;
            label[n_label].y1 = letters[lbond[i].a].y;
            label[n_label].r1 = letters[lbond[i].a].r;
            found_left = true;
          }
        if (!isdigit(letters[lbond[i].b].a) && letters[lbond[i].b].a != '-' && letters[lbond[i].b].a != '+'
            && !found_left)
          {
            label[n_label].x1 = letters[lbond[i].b].x;
            label[n_label].y1 = letters[lbond[i].b].y;
            label[n_label].r1 = letters[lbond[i].b].r;
            found_left = true;
          }
        if (!isdigit(letters[lbond[i].a].a) && letters[lbond[i].a].a != '-' && letters[lbond[i].a].a != '+')
          {
            label[n_label].x2 = letters[lbond[i].a].x;
            label[n_label].y2 = letters[lbond[i].a].y;
            label[n_label].r2 = letters[lbond[i].a].r;
          }
        if (!isdigit(letters[lbond[i].b].a) && letters[lbond[i].b].a != '-' && letters[lbond[i].b].a != '+')
          {
            label[n_label].x2 = letters[lbond[i].b].x;
            label[n_label].y2 = letters[lbond[i].b].y;
            label[n_label].r2 = letters[lbond[i].b].r;
          }
        lbond[i].exists = false;
        int last = lbond[i].b;
        for (int j = i + 1; j < n_lbond; j++)
          if ((lbond[j].exists) && (lbond[j].a == last))
            {
              label[n_label].a += letters[lbond[j].b].a;
              label[n_label].n.push_back(lbond[j].b);
              if (!isdigit(letters[lbond[j].a].a) && letters[lbond[j].a].a != '-' && letters[lbond[j].a].a != '+'
                  && !found_left)
                {
                  label[n_label].x1 = letters[lbond[j].a].x;
                  label[n_label].y1 = letters[lbond[j].a].y;
                  label[n_label].r1 = letters[lbond[j].a].r;
                  found_left = true;
                }
              if (!isdigit(letters[lbond[j].b].a) && letters[lbond[j].b].a != '-' && letters[lbond[j].b].a != '+'
                  && !found_left)
                {
                  label[n_label].x1 = letters[lbond[j].b].x;
                  label[n_label].y1 = letters[lbond[j].b].y;
                  label[n_label].r1 = letters[lbond[j].b].r;
                  found_left = true;
                }
              if (!isdigit(letters[lbond[j].a].a) && letters[lbond[j].a].a != '-' && letters[lbond[j].a].a != '+')
                {
                  label[n_label].x2 = letters[lbond[j].a].x;
                  label[n_label].y2 = letters[lbond[j].a].y;
                  label[n_label].r2 = letters[lbond[j].a].r;
                }
              if (!isdigit(letters[lbond[j].b].a) && letters[lbond[j].b].a != '-' && letters[lbond[j].b].a != '+')
                {
                  label[n_label].x2 = letters[lbond[j].b].x;
                  label[n_label].y2 = letters[lbond[j].b].y;
                  label[n_label].r2 = letters[lbond[j].b].r;
                }
              last = lbond[j].b;
              lbond[j].exists = false;
            }

        n_label++;
        if (n_label >= MAX_ATOMS)
          n_label--;
      }

  int old_n_label = n_label;
  for (int i = 0; i < old_n_label; i++)
    {
      double cy = 0;
      int n = 0;

      for (unsigned int j = 0; j < label[i].n.size(); j++)
        if (isalpha(letters[label[i].n[j]].a))
          {
            cy += letters[label[i].n[j]].y;
            n++;
          }
      cy /= n;
      n = 0;
      for (unsigned int j = 0; j < label[i].n.size(); j++)
        if (isalpha(letters[label[i].n[j]].a) && letters[label[i].n[j]].y - cy > letters[label[i].n[j]].r / 2)
          n++;

      if (n > 1)
        {
          label[i].a = "";
          label[i].x1 = FLT_MAX;
          label[i].x2 = 0;
          label_t lb;
          label.push_back(lb);
          label[n_label].a = "";
          label[n_label].x1 = FLT_MAX;
          label[n_label].x2 = 0;

          for (unsigned int j = 0; j < label[i].n.size(); j++)
            {
              if (letters[label[i].n[j]].y > cy)
                {
                  label[i].a += letters[label[i].n[j]].a;
                  if (isalpha(letters[label[i].n[j]].a))
                    {
                      if (letters[label[i].n[j]].x < label[i].x1)
                        {
                          label[i].x1 = letters[label[i].n[j]].x;
                          label[i].y1 = letters[label[i].n[j]].y;
                          label[i].r1 = letters[label[i].n[j]].r;
                        }
                      if (letters[label[i].n[j]].x > label[i].x2)
                        {
                          label[i].x2 = letters[label[i].n[j]].x;
                          label[i].y2 = letters[label[i].n[j]].y;
                          label[i].r2 = letters[label[i].n[j]].r;
                        }
                    }
                }
              else
                {
                  label[n_label].a += letters[label[i].n[j]].a;
                  if (isalpha(letters[label[i].n[j]].a))
                    {
                      if (letters[label[i].n[j]].x < label[n_label].x1)
                        {
                          label[n_label].x1 = letters[label[i].n[j]].x;
                          label[n_label].y1 = letters[label[i].n[j]].y;
                          label[n_label].r1 = letters[label[i].n[j]].r;
                        }
                      if (letters[label[i].n[j]].x > label[n_label].x2)
                        {
                          label[n_label].x2 = letters[label[i].n[j]].x;
                          label[n_label].y2 = letters[label[i].n[j]].y;
                          label[n_label].r2 = letters[label[i].n[j]].r;
                        }
                    }
                }
            }
          n_label++;
        }
    }

  for (int i = 0; i < n_label; i++)
    {
      bool cont = true;
      string charges = "";
      while (cont)
        {
          cont = false;
          string::size_type pos = label[i].a.find_first_of('-');
          if (pos != string::npos)
            {
              label[i].a.erase(pos, 1);
              charges += "-";
              cont = true;
            }
          pos = label[i].a.find_first_of('+');
          if (pos != string::npos)
            {
              label[i].a.erase(pos, 1);
              charges += "+";
              cont = true;
            }
        }
      label[i].a += charges;
    }

  return (n_label);
}

void extend_terminal_bond_to_label(vector<atom_t> &atom, const vector<letters_t> &letters, int n_letters, const vector<
                                   bond_t> &bond, int n_bond, const vector<label_t> &label, int n_label, double avg, double maxh,
                                   double max_dist_double_bond)
{
  for (int j = 0; j < n_bond; j++)
    if (bond[j].exists)
      {
        bool not_corner_a = terminal_bond(bond[j].a, j, bond, n_bond);
        bool not_corner_b = terminal_bond(bond[j].b, j, bond, n_bond);
        if (atom[bond[j].a].label != " ")
          not_corner_a = false;
        if (atom[bond[j].b].label != " ")
          not_corner_b = false;
        double xa = atom[bond[j].a].x;
        double ya = atom[bond[j].a].y;
        double xb = atom[bond[j].b].x;
        double yb = atom[bond[j].b].y;
        double bl = bond_length(bond, j, atom);
        double minb = FLT_MAX;
        bool found1 = false, found2 = false;
        int l1 = -1, l2 = -1;
        if (not_corner_a)
          {
            for (int i = 0; i < n_label; i++)
              if ((label[i].a)[0] != '+' && (label[i].a)[0] != '-')
                {
                  double d1 = distance_from_bond_x_a(xa, ya, xb, yb, label[i].x1, label[i].y1);
                  double d2 = distance_from_bond_x_a(xa, ya, xb, yb, label[i].x2, label[i].y2);
                  double h1 = fabs(distance_from_bond_y(xa, ya, xb, yb, label[i].x1, label[i].y1));
                  double h2 = fabs(distance_from_bond_y(xa, ya, xb, yb, label[i].x2, label[i].y2));
                  double y_dist = maxh + label[i].r1 / 2;
                  if (bond[j].type > 1)
                    y_dist += max_dist_double_bond;
                  double nb = fabs(d1) - label[i].r1;
                  if (nb <= avg && h1 <= y_dist && nb < minb && d1 < bl / 2)
                    {
                      found1 = true;
                      l1 = i;
                      minb = nb;
                    }
                  y_dist = maxh + label[i].r2 / 2;
                  if (bond[j].type > 1)
                    y_dist += max_dist_double_bond;
                  nb = fabs(d2) - label[i].r2;
                  if (nb <= avg && h2 <= y_dist && nb < minb && d2 < bl / 2)
                    {
                      found1 = true;
                      l1 = i;
                      minb = nb;
                    }
                }
            for (int i = 0; i < n_letters; i++)
              if (letters[i].free && letters[i].a != '+' && letters[i].a != '-')
                {
                  double d = distance_from_bond_x_a(xa, ya, xb, yb, letters[i].x, letters[i].y);
                  double y_dist = maxh + letters[i].r / 2;
                  if (bond[j].type > 1)
                    y_dist += max_dist_double_bond;
                  double h = fabs(distance_from_bond_y(xa, ya, xb, yb, letters[i].x, letters[i].y));
                  double nb = fabs(d) - letters[i].r;
                  if (nb <= avg && h <= y_dist && nb < minb && d < bl / 2)
                    {
                      found2 = true;
                      l2 = i;
                      minb = nb;
                    }
                }
            if (found2)
              {
                atom[bond[j].a].label = toupper(letters[l2].a);
                atom[bond[j].a].x = letters[l2].x;
                atom[bond[j].a].y = letters[l2].y;
              }
            else if (found1)
              {
                atom[bond[j].a].label = label[l1].a;
                atom[bond[j].a].x = (label[l1].x1 + label[l1].x2) / 2;
                atom[bond[j].a].y = (label[l1].y1 + label[l1].y2) / 2;
              }
          }
        if (not_corner_b)
          {
            found1 = false, found2 = false;
            minb = FLT_MAX;
            for (int i = 0; i < n_label; i++)
              if ((label[i].a)[0] != '+' && (label[i].a)[0] != '-' && i != l1)
                {
                  double d1 = distance_from_bond_x_b(xa, ya, xb, yb, label[i].x1, label[i].y1);
                  double d2 = distance_from_bond_x_b(xa, ya, xb, yb, label[i].x2, label[i].y2);
                  double h1 = fabs(distance_from_bond_y(xa, ya, xb, yb, label[i].x1, label[i].y1));
                  double h2 = fabs(distance_from_bond_y(xa, ya, xb, yb, label[i].x2, label[i].y2));
                  double y_dist = maxh + label[i].r1 / 2;
                  if (bond[j].type > 1)
                    y_dist += max_dist_double_bond;
                  double nb = fabs(d1) - label[i].r1; // end "b" and 1st side
                  if (nb <= avg && h1 <= y_dist && nb < minb && d1 > -bl / 2)
                    {
                      found1 = true;
                      l1 = i;
                      minb = nb;
                    }
                  y_dist = maxh + label[i].r2 / 2;
                  if (bond[j].type > 1)
                    y_dist += max_dist_double_bond;
                  nb = fabs(d2) - label[i].r2; // end "b" and 2nd side
                  if (nb <= avg && h2 <= y_dist && nb < minb && d2 > -bl / 2)
                    {
                      found1 = true;
                      l1 = i;
                      minb = nb;
                    }
                }
            for (int i = 0; i < n_letters; i++)
              if (letters[i].free && letters[i].a != '+' && letters[i].a != '-' && i != l2)
                {
                  double d = distance_from_bond_x_b(xa, ya, xb, yb, letters[i].x, letters[i].y);
                  double nb = fabs(d) - letters[i].r; // distance between end "b" and letter
                  double y_dist = maxh + letters[i].r / 2;
                  if (bond[j].type > 1)
                    y_dist += max_dist_double_bond;
                  double h = fabs(distance_from_bond_y(xa, ya, xb, yb, letters[i].x, letters[i].y));
                  if (nb <= avg && h <= y_dist && nb < minb && d > -bl / 2)
                    {
                      found2 = true;
                      l2 = i;
                      minb = nb;
                    }
                }

            if (found2)
              {
                atom[bond[j].b].label = toupper(letters[l2].a);
                ;
                atom[bond[j].b].x = letters[l2].x;
                atom[bond[j].b].y = letters[l2].y;
              }
            else if (found1)
              {
                atom[bond[j].b].label = label[l1].a;
                atom[bond[j].b].x = (label[l1].x1 + label[l1].x2) / 2;
                atom[bond[j].b].y = (label[l1].y1 + label[l1].y2) / 2;
              }
          }
      }
}

void extend_terminal_bond_to_bonds(vector<atom_t> &atom, vector<bond_t> &bond, int n_bond, double avg, double maxh,
                                   double max_dist_double_bond)
{
  bool found_intersection = true;

  while (found_intersection)
    {
      found_intersection = false;
      for (int j = 0; j < n_bond; j++)
        if (bond[j].exists)
          {
            bool not_corner_a = terminal_bond(bond[j].a, j, bond, n_bond);
            bool not_corner_b = terminal_bond(bond[j].b, j, bond, n_bond);
            double xa = atom[bond[j].a].x;
            double ya = atom[bond[j].a].y;
            double xb = atom[bond[j].b].x;
            double yb = atom[bond[j].b].y;
            double bl = bond_length(bond, j, atom);
            double minb = FLT_MAX;
            bool found = false;
            int l = -1;
            for (int i = 0; i < n_bond; i++)
              if (bond[i].exists && i != j)
                if (not_corner_a)
                  {
                    double h1 = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                          atom[bond[i].b].x, atom[bond[i].b].y, xa, ya));

                    double y_dist = maxh;
                    double y_dist1 = maxh;

                    if (bond[j].type > 1 && !bond[j].conjoined)
                      y_dist += max_dist_double_bond;
                    if (bond[i].type > 1 && !bond[i].conjoined)
                      y_dist1 += max_dist_double_bond;

                    int ai = bond[i].a;
                    if (ai != bond[j].a && ai != bond[j].b)
                      {
                        double d = distance_from_bond_x_a(xa, ya, xb, yb, atom[ai].x, atom[ai].y);
                        double h = fabs(distance_from_bond_y(xa, ya, xb, yb, atom[ai].x, atom[ai].y));
                        if (fabs(d) <= avg / 2 && h <= y_dist && fabs(d) < minb && d < bl / 2 && h1 < y_dist1)
                          {
                            found = true;
                            l = ai;
                            minb = fabs(d);
                          }
                      }
                    int bi = bond[i].b;
                    if (bi != bond[j].a && bi != bond[j].b)
                      {
                        double d = distance_from_bond_x_a(xa, ya, xb, yb, atom[bi].x, atom[bi].y);
                        double h = fabs(distance_from_bond_y(xa, ya, xb, yb, atom[bi].x, atom[bi].y));
                        if (fabs(d) <= avg / 2 && h <= y_dist && fabs(d) < minb && d < bl / 2 && h1 < y_dist1)
                          {
                            found = true;
                            l = bi;
                            minb = fabs(d);
                          }
                      }
                  }
            if (found)
              {
                atom[l].x = (atom[bond[j].a].x + atom[l].x) / 2;
                atom[l].y = (atom[bond[j].a].y + atom[l].y) / 2;
                bond[j].a = l;
                found_intersection = true;
              }

            found = false;
            minb = FLT_MAX;
            l = -1;
            for (int i = 0; i < n_bond; i++)
              if (bond[i].exists && i != j)
                if (not_corner_b)
                  {
                    double h1 = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                          atom[bond[i].b].x, atom[bond[i].b].y, xb, yb));
                    double y_dist = maxh;
                    double y_dist1 = maxh;
                    if (bond[j].type > 1 && !bond[j].conjoined)
                      y_dist += max_dist_double_bond;
                    if (bond[i].type > 1 && !bond[i].conjoined)
                      y_dist1 += max_dist_double_bond;

                    int ai = bond[i].a;
                    if (ai != bond[j].a && ai != bond[j].b)
                      {
                        double d = distance_from_bond_x_b(xa, ya, xb, yb, atom[ai].x, atom[ai].y);
                        double h = fabs(distance_from_bond_y(xa, ya, xb, yb, atom[ai].x, atom[ai].y));
                        if (fabs(d) <= avg / 2 && h <= y_dist && fabs(d) < minb && d > -bl / 2 && h1 < y_dist1)
                          {
                            found = true;
                            l = ai;
                            minb = fabs(d);
                          }
                      }
                    int bi = bond[i].b;
                    if (bi != bond[j].a && bi != bond[j].b)
                      {
                        double d = distance_from_bond_x_b(xa, ya, xb, yb, atom[bi].x, atom[bi].y);
                        double h = fabs(distance_from_bond_y(xa, ya, xb, yb, atom[bi].x, atom[bi].y));
                        if (fabs(d) <= avg / 2 && h <= y_dist && fabs(d) < minb && d > -bl / 2 && h1 < y_dist1)
                          {
                            found = true;
                            l = bi;
                            minb = fabs(d);
                          }
                      }
                  }

            if (found)
              {
                atom[l].x = (atom[bond[j].b].x + atom[l].x) / 2;
                atom[l].y = (atom[bond[j].b].y + atom[l].y) / 2;
                bond[j].b = l;
                found_intersection = true;
              }
          }
    }
}

void assign_charge(vector<atom_t> &atom, vector<bond_t> &bond, int n_atom, int n_bond, const map<string, string> &fix,
                   const map<string, string> &superatom, bool debug)
{
  for (int j = 0; j < n_bond; j++)
    if (bond[j].exists && (!atom[bond[j].a].exists || !atom[bond[j].b].exists))
      bond[j].exists = false;

  for (int i = 0; i < n_atom; i++)
    if (atom[i].exists)
      {
        int n = 0;
        int m = 0;
        for (int j = 0; j < n_bond; j++)
          if (bond[j].exists && (bond[j].a == i || bond[j].b == i))
            {
              n += bond[j].type;
              if (bond[j].type > 1)
                m++;
            }
        atom[i].charge = 0;
        bool cont = true;
        while (cont)
          {
            string::size_type pos = atom[i].label.find_first_of('-');
            if (pos != string::npos)
              {
                atom[i].label.erase(pos, 1);
                if (atom[i].label.length() > 0 && isalpha(atom[i].label.at(0)))
                  atom[i].charge--;
              }
            else
              {
                pos = atom[i].label.find_first_of('+');
                if (pos != string::npos)
                  {
                    atom[i].label.erase(pos, 1);
                    if (atom[i].label.length() > 0 && isalpha(atom[i].label.at(0)))
                      atom[i].charge++;
                  }
                else
                  cont = false;
              }
          }
        for (int j = 0; j < n_bond; j++)
          if (bond[j].exists && bond[j].hash && bond[j].b == i)
            atom[i].charge = 0;

        atom[i].label = fix_atom_name(atom[i].label, n, fix, superatom, debug);
      }
}


void debug_img(Image &image, const vector<atom_t> &atom, int n_atom, const vector<bond_t> &bond, int n_bond,
               const string &fname)
{
  image.modifyImage();
  image.type(TrueColorType);
  image.strokeWidth(1);

  int max_x = image.columns();
  int max_y = image.rows();

  for (int i = 0; i < n_bond; i++)
    {
      if ((bond[i].exists) && (atom[bond[i].a].exists) && (atom[bond[i].b].exists))
        {
          if (bond[i].type == 1)
            {
              image.strokeColor("green");
            }
          else if (bond[i].type == 2)
            {
              image.strokeColor("yellow");
            }
          else if (bond[i].type >= 3)
            {
              image.strokeColor("red");
            }
          if (bond[i].hash)
            {
              image.strokeColor("blue");
            }
          else if (bond[i].wedge)
            {
              image.strokeColor("purple");
            }
          image.draw(DrawableLine(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x, atom[bond[i].b].y));
        }
    }

  for (int i = 0; i < n_atom; i++)
    {
      if (atom[i].exists)
        {
          if ((int(atom[i].x) < max_x) && (int(atom[i].y < max_y)))
            image.pixelColor(int(atom[i].x), int(atom[i].y), "blue");
        }
    }

  image.write(fname);
}

void draw_square(Image &image, int x1, int y1, int x2, int y2, const string &color)
{
  image.strokeWidth(1);
  image.strokeColor(color);
  image.draw(DrawableLine(x1, y1, x2, y1));
  image.draw(DrawableLine(x1, y2, x2, y2));
  image.draw(DrawableLine(x1, y1, x1, y2));
  image.draw(DrawableLine(x2, y1, x2, y2));
}

void draw_box(Image &image, vector<box_t> &boxes, int n_boxes, const string &fname)
{
  image.modifyImage();
  image.type(TrueColorType);

  for (int i = 0; i < n_boxes; i++)
    {
      draw_square(image, boxes[i].x1, boxes[i].y1, boxes[i].x2, boxes[i].y2, "green");
    }
  image.write(fname);
}

int next_atom(int cur, int begin, int total)
{
  int n = cur + 1;
  if (n > total - 1)
    {
      n = begin;
    }
  return (n);
}

bool dir_change(int n, int last, int begin, int total, const vector<atom_t> &atom)
{
  int m = next_atom(n, begin, total);
  while (distance(atom[m].x, atom[m].y, atom[n].x, atom[n].y) < V_DISPLACEMENT && m != n)
    m = next_atom(m, begin, total);
  if (m == n)
    return (false);
  double s = fabs(distance_from_bond_y(atom[n].x, atom[n].y, atom[last].x, atom[last].y, atom[m].x, atom[m].y));
  if (s > DIR_CHANGE)
    return (true);
  return (false);
}

bool smaller_distance(int n, int last, int begin, int total, const vector<atom_t> &atom)
{
  int m = next_atom(n, begin, total);
  double d1 = distance(atom[n].x, atom[n].y, atom[last].x, atom[last].y);
  double d2 = distance(atom[m].x, atom[m].y, atom[last].x, atom[last].y);
  if (d1 > d2)
    {
      return (true);
    }
  return (false);
}

int find_bonds(vector<atom_t> &atom, vector<bond_t> &bond, int b_atom, int n_atom, int n_bond,
               const potrace_path_t * const p)
{
  int i = b_atom + 1;
  int last = b_atom;

  while (i < n_atom)
    {
      if (atom[i].corner)
        {
          atom[i].exists = true;
          last = i;
          i++;
        }
      else if (dir_change(i, last, b_atom, n_atom, atom))
        {
          atom[i].exists = true;
          last = i;
          i++;
        }
      else if (smaller_distance(i, last, b_atom, n_atom, atom))
        {
          atom[i].exists = true;
          last = i;
          i++;
        }
      else
        {
          i++;
        }
    }
  for (i = b_atom; i < n_atom; i++)
    if (atom[i].exists)
      {
        bond_t bn;
        bond.push_back(bn);
        bond[n_bond].a = i;
        bond[n_bond].exists = true;
        bond[n_bond].type = 1;
        int j = next_atom(i, b_atom, n_atom);
        while (!atom[j].exists)
          {
            j = next_atom(j, b_atom, n_atom);
          }
        bond[n_bond].b = j;
        bond[n_bond].curve = p;
        bond[n_bond].hash = false;
        bond[n_bond].wedge = false;
        bond[n_bond].up = false;
        bond[n_bond].down = false;
        bond[n_bond].Small = false;
        bond[n_bond].arom = false;
        bond[n_bond].conjoined = false;
        n_bond++;
        if (n_bond >= MAX_ATOMS)
          n_bond--;
      }
  return (n_bond);
}

char get_atom_label_unpaper(const Magick::Image &orig, const Magick::ColorGray &bgColor, int left, int top, int right, int bottom,
                            double THRESHOLD, int dropx, int dropy, bool verbose)
{
  char label = 0;
  ColorGray g;
  Image tmp(Geometry(right-left+1,bottom-top+1), bgColor);
  tmp.type(GrayscaleType);

  for (int x=left; x<=right; x++)
    for (int y=top; y<=bottom; y++)
      {
        g = orig.pixelColor(x,y);
        tmp.pixelColor(x-left, y-top,g);
      }
  right=tmp.columns();
  bottom=tmp.rows();
  left=0;
  top=0;
  label = get_atom_label(tmp, bgColor, left, top, right, bottom, THRESHOLD, (right + left) / 2, top, verbose);
  if (label == 0)
    {
      unpaper(tmp);
      label = get_atom_label(tmp, bgColor, left, top, right, bottom, THRESHOLD, (right + left) / 2, top, verbose);
    }
  if (label == 0)
    {
      unpaper(tmp);
      label = get_atom_label(tmp, bgColor, left, top, right, bottom, THRESHOLD, (right + left) / 2, top, verbose);
    }
  return(label);
}

int find_chars(const potrace_path_t * p, const Image &orig, vector<letters_t> &letters, vector<atom_t> &atom, vector<
               bond_t> &bond, int n_atom, int n_bond, int height, int width, ColorGray &bgColor, double THRESHOLD,
               int max_font_width, int max_font_height, int &real_font_width, int &real_font_height, bool verbose)
{
  int n, *tag, n_letters = 0;
  potrace_dpoint_t (*c)[3];
  real_font_width = 0;
  real_font_height = 0;

  while (p != NULL)
    {
      if ((p->sign == int('+')))
        {
          n = p->curve.n;
          tag = p->curve.tag;
          c = p->curve.c;
          int top = height;
          int x1 = 0;
          int left = width;
          int y1 = 0;
          int bottom = 0;
          int x2 = 0;
          int right = 0;
          int y2 = 0;
          for (int i = 0; i < n; i++)
            {
              switch (tag[i])
                {
                case POTRACE_CORNER:
                  if (c[i][1].x < left)
                    {
                      left = int(c[i][1].x);
                      y1 = int(c[i][1].y);
                    }
                  if (c[i][1].x > right)
                    {
                      right = int(c[i][1].x);
                      y2 = int(c[i][1].y);
                    }
                  if (c[i][1].y < top)
                    {
                      top = int(c[i][1].y);
                      x1 = int(c[i][1].x);
                    }
                  if (c[i][1].y > bottom)
                    {
                      bottom = int(c[i][1].y);
                      x2 = int(c[i][1].x);
                    }
                  break;
                case POTRACE_CURVETO:
                  if (c[i][0].x < left)
                    {
                      left = int(c[i][0].x);
                      y1 = int(c[i][0].y);
                    }
                  if (c[i][0].x > right)
                    {
                      right = int(c[i][0].x);
                      y2 = int(c[i][0].y);
                    }
                  if (c[i][0].y < top)
                    {
                      top = int(c[i][0].y);
                      x1 = int(c[i][0].x);
                    }
                  if (c[i][0].y > bottom)
                    {
                      bottom = int(c[i][0].y);
                      x2 = int(c[i][0].x);
                    }
                  if (c[i][1].x < left)
                    {
                      left = int(c[i][1].x);
                      y1 = int(c[i][1].y);
                    }
                  if (c[i][1].x > right)
                    {
                      right = int(c[i][1].x);
                      y2 = int(c[i][1].y);
                    }
                  if (c[i][1].y < top)
                    {
                      top = int(c[i][1].y);
                      x1 = int(c[i][1].x);
                    }
                  if (c[i][1].y > bottom)
                    {
                      bottom = int(c[i][1].y);
                      x2 = int(c[i][1].x);
                    }
                  break;
                }
              if (c[i][2].x < left)
                {
                  left = int(c[i][2].x);
                  y1 = int(c[i][2].y);
                }
              if (c[i][2].x > right)
                {
                  right = int(c[i][2].x);
                  y2 = int(c[i][2].y);
                }
              if (c[i][2].y < top)
                {
                  top = int(c[i][2].y);
                  x1 = int(c[i][2].x);
                }
              if (c[i][2].y > bottom)
                {
                  bottom = int(c[i][2].y);
                  x2 = int(c[i][2].x);
                }
            }

          if (((bottom - top) <= 2 * max_font_height) && ((right - left) <= 2 * max_font_width) && (right - left
              > V_DISPLACEMENT) && (bottom - top > MIN_FONT_HEIGHT))
            {
              int s = 1;
              while ((top > 0) && (s > 0))
                {
                  s = 0;
                  s = get_pixel(orig, bgColor, x1, top, THRESHOLD);
                  if (s > 0)
                    top--;
                }
              s = 1;
              while ((bottom < height) && (s > 0))
                {
                  s = 0;
                  s = get_pixel(orig, bgColor, x2, bottom, THRESHOLD);
                  if (s > 0)
                    bottom++;
                }
              s = 1;
              while ((left > 0) && (s > 0))
                {
                  s = 0;
                  s = get_pixel(orig, bgColor, left, y1, THRESHOLD);
                  if (s > 0)
                    left--;
                }
              s = 1;
              while ((right < width) && (s > 0))
                {
                  s = 0;
                  s = get_pixel(orig, bgColor, right, y2, THRESHOLD);
                  if (s > 0)
                    right++;
                }
            }

          bool found = false;
          if (((bottom - top) <= max_font_height) && ((right - left) <= max_font_width) && (right - left
              > V_DISPLACEMENT) && (bottom - top > MIN_FONT_HEIGHT))
            {

              char label = 0;
              label = get_atom_label(orig, bgColor, left, top, right, bottom, THRESHOLD, (right + left) / 2, top, verbose);

              if (label != 0)
                {
                  letters_t lt;
                  letters.push_back(lt);
                  letters[n_letters].a = label;
                  letters[n_letters].x = (left + right) / 2;
                  letters[n_letters].y = (top + bottom) / 2;
                  letters[n_letters].r = distance(left, top, right, bottom) / 2;
                  if (right - left > real_font_width)
                    real_font_width = right - left;
                  if (bottom - top > real_font_height)
                    real_font_height = bottom - top;
                  letters[n_letters].free = true;
                  n_letters++;
                  if (n_letters >= MAX_ATOMS)
                    n_letters--;
                  delete_bonds_in_char(bond, n_bond, atom, left, top, right, bottom);
                  delete_curve_with_children(atom, bond, n_atom, n_bond, p);
                  found = true;
                }
            }
          if (((bottom - top) <= 2 * max_font_height) && ((right - left) <= max_font_width) && (right - left
              > V_DISPLACEMENT) && (bottom - top > MIN_FONT_HEIGHT) && !found)
            {

              char label1 = 0;
              int newtop = (top + bottom) / 2;
              label1 = get_atom_label(orig, bgColor, left, newtop, right, bottom, THRESHOLD, (right + left) / 2,
                                      newtop, verbose);
              char label2 = 0;
              int newbottom = (top + bottom) / 2;
              label2 = get_atom_label(orig, bgColor, left, top, right, newbottom, THRESHOLD, (right + left) / 2, top, verbose);
              if ((label1 != 0) && (label2 != 0))
                {
                  //cout << label1 << label2 << endl;
                  letters_t lt1;
                  letters.push_back(lt1);
                  letters[n_letters].a = label1;
                  letters[n_letters].x = (left + right) / 2;
                  letters[n_letters].y = (newtop + bottom) / 2;
                  letters[n_letters].r = distance(left, newtop, right, bottom) / 2;
                  if (right - left > real_font_width)
                    real_font_width = right - left;
                  if (bottom - newtop > real_font_height)
                    real_font_height = bottom - newtop;
                  letters[n_letters].free = true;
                  n_letters++;
                  if (n_letters >= MAX_ATOMS)
                    n_letters--;
                  letters_t lt2;
                  letters.push_back(lt2);
                  letters[n_letters].a = label2;
                  letters[n_letters].x = (left + right) / 2;
                  letters[n_letters].y = (top + newbottom) / 2;
                  letters[n_letters].r = distance(left, top, right, newbottom) / 2;
                  if (newbottom - top > real_font_height)
                    real_font_height = newbottom - top;
                  letters[n_letters].free = true;
                  n_letters++;
                  if (n_letters >= MAX_ATOMS)
                    n_letters--;
                  delete_bonds_in_char(bond, n_bond, atom, left, top, right, bottom);
                  delete_curve_with_children(atom, bond, n_atom, n_bond, p);
                  found = true;
                }
            }
          if (((bottom - top) <= max_font_height) && ((right - left) <= 2 * max_font_width) && (right - left
              > V_DISPLACEMENT) && (bottom - top > MIN_FONT_HEIGHT) && !found)
            {

              char label1 = 0;
              int newright = (left + right) / 2;
              label1 = get_atom_label(orig, bgColor, left, top, newright, bottom, THRESHOLD, (left + newright) / 2,
                                      top, verbose);
              char label2 = 0;
              int newleft = (left + right) / 2;
              label2 = get_atom_label(orig, bgColor, newleft, top, right, bottom, THRESHOLD, (newleft + right) / 2,
                                      top, verbose);
              if ((label1 != 0) && (label2 != 0))
                {
                  //cout << label1 << label2 << endl;
                  letters_t lt1;
                  letters.push_back(lt1);
                  letters[n_letters].a = label1;
                  letters[n_letters].x = (left + newright) / 2;
                  letters[n_letters].y = (top + bottom) / 2;
                  letters[n_letters].r = distance(left, top, newright, bottom) / 2;
                  if (newright - left > real_font_width)
                    real_font_width = newright - left;
                  if (bottom - top > real_font_height)
                    real_font_height = bottom - top;
                  letters[n_letters].free = true;
                  n_letters++;
                  if (n_letters >= MAX_ATOMS)
                    n_letters--;
                  letters_t lt2;
                  letters.push_back(lt2);
                  letters[n_letters].a = label2;
                  letters[n_letters].x = (newleft + right) / 2;
                  letters[n_letters].y = (top + bottom) / 2;
                  letters[n_letters].r = distance(newleft, top, right, bottom) / 2;
                  if (right - newleft > real_font_width)
                    real_font_width = right - newleft;
                  letters[n_letters].free = true;
                  n_letters++;
                  if (n_letters >= MAX_ATOMS)
                    n_letters--;
                  delete_bonds_in_char(bond, n_bond, atom, left, top, right, bottom);
                  delete_curve_with_children(atom, bond, n_atom, n_bond, p);
                  found = true;
                }
            }

        }
      p = p->next;
    }
  if (real_font_width < 1)
    real_font_width = max_font_width;
  else
    real_font_width++;
  if (real_font_height < 1)
    real_font_height = max_font_height;
  else
    real_font_height++;
  return (n_letters);
}

int find_atoms(const potrace_path_t *p, vector<atom_t> &atom, vector<bond_t> &bond, int *n_bond)
{
  int *tag, n_atom = 0;
  potrace_dpoint_t (*c)[3];
  long n;

  while (p != NULL)
    {
      n = p->curve.n;
      tag = p->curve.tag;
      c = p->curve.c;
      int b_atom = n_atom;
      atom_t at;
      atom.push_back(at);
      atom[n_atom].x = c[n - 1][2].x;
      atom[n_atom].y = c[n - 1][2].y;
      atom[n_atom].label = " ";
      atom[n_atom].exists = false;
      atom[n_atom].curve = p;
      atom[n_atom].n = 0;
      atom[n_atom].corner = false;
      atom[n_atom].terminal = false;
      atom[n_atom].charge = 0;
      atom[n_atom].anum = 0;
      n_atom++;
      if (n_atom >= MAX_ATOMS)
        n_atom--;
      for (long i = 0; i < n; i++)
        {
          atom_t at1, at2, at3, at4;

          switch (tag[i])
            {
            case POTRACE_CORNER:
              atom.push_back(at1);
              atom[n_atom].x = c[i][1].x;
              atom[n_atom].y = c[i][1].y;
              atom[n_atom].label = " ";
              atom[n_atom].exists = false;
              atom[n_atom].curve = p;
              atom[n_atom].n = 0;
              atom[n_atom].corner = true;
              atom[n_atom].terminal = false;
              atom[n_atom].charge = 0;
              atom[n_atom].anum = 0;
              n_atom++;
              if (n_atom >= MAX_ATOMS)
                n_atom--;
              break;
            case POTRACE_CURVETO:
              atom.push_back(at2);
              atom[n_atom].x = c[i][0].x;
              atom[n_atom].y = c[i][0].y;
              atom[n_atom].label = " ";
              atom[n_atom].exists = false;
              atom[n_atom].curve = p;
              atom[n_atom].n = 0;
              atom[n_atom].corner = false;
              atom[n_atom].terminal = false;
              atom[n_atom].charge = 0;
              atom[n_atom].anum = 0;
              n_atom++;
              if (n_atom >= MAX_ATOMS)
                n_atom--;
              atom.push_back(at3);
              atom[n_atom].x = c[i][1].x;
              atom[n_atom].y = c[i][1].y;
              atom[n_atom].label = " ";
              atom[n_atom].exists = false;
              atom[n_atom].curve = p;
              atom[n_atom].n = 0;
              atom[n_atom].corner = false;
              atom[n_atom].terminal = false;
              atom[n_atom].charge = 0;
              atom[n_atom].anum = 0;
              n_atom++;
              if (n_atom >= MAX_ATOMS)
                n_atom--;
              break;
            }
          if (i != n - 1)
            {
              atom.push_back(at4);
              atom[n_atom].x = c[i][2].x;
              atom[n_atom].y = c[i][2].y;
              atom[n_atom].label = " ";
              atom[n_atom].exists = false;
              atom[n_atom].curve = p;
              atom[n_atom].n = 0;
              atom[n_atom].corner = false;
              atom[n_atom].terminal = false;
              atom[n_atom].charge = 0;
              atom[n_atom].anum = 0;
              n_atom++;
              if (n_atom >= MAX_ATOMS)
                n_atom--;
            }
        }
      *n_bond = find_bonds(atom, bond, b_atom, n_atom, *n_bond, p);
      p = p->next;
    }
  return (n_atom);
}

int count_pages(const string &input)
{
  list<Image> imageList;
  readImages(&imageList, input);
  return (imageList.size());
}

int count_atoms(const vector<atom_t> &atom, int n_atom)
{
  int r = 0;
  for (int i = 0; i < n_atom; i++)
    if (atom[i].exists)
      r++;
  return (r);
}

int count_bonds(const vector<bond_t> &bond, int n_bond, int &bond_max_type)
{
  int r = 0;
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      {
	r++;
	if (bond[i].type>bond_max_type) bond_max_type = bond[i].type;
      }
  return (r);
}

/*------------------- ThinImage - Thin binary image. --------------------------- *
 *
 *    Description:
 *        Thins the supplied binary image using Rosenfeld's parallel
 *        thinning algorithm.
 *
 *    On Entry:
 *        image = Image to thin.
 *
 *------------------------------------------------------------------------------- */

/* Direction masks:                  */
/*   N     S     W        E            */
static unsigned int masks[] = { 0200, 0002, 0040, 0010 };

/*    True if pixel neighbor map indicates the pixel is 8-simple and  */
/*    not an end point and thus can be deleted.  The neighborhood     */
/*    map is defined as an integer of bits abcdefghi with a non-zero  */
/*    bit representing a non-zero pixel.  The bit assignment for the  */
/*    neighborhood is:                                                */
/*                                                                    */
/*                            a b c                                   */
/*                            d e f                                   */
/*                            g h i                                   */

static unsigned char todelete[512] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1,
                                       1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
                                       0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1,
                                       1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
                                       1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                                       0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1,
                                       1, 1, 1, 1
                                     };

void thin1(unsigned char * const ptr, unsigned int xsize, unsigned int ysize)
{
  unsigned char *y_ptr, *y1_ptr;
  unsigned char bg_color = 0, colour = 1;
  unsigned int x, y; /* Pixel location               */
  unsigned int i; /* Pass index           */
  unsigned int pc = 0; /* Pass count           */
  unsigned int count = 1; /* Deleted pixel count          */
  unsigned int p, q; /* Neighborhood maps of adjacent*/
  /* cells                        */
  unsigned char *qb; /* Neighborhood maps of previous*/
  /* scanline                     */
  unsigned int m; /* Deletion direction mask      */

  qb = (unsigned char*) malloc(xsize * sizeof(unsigned char));
  qb[xsize - 1] = 0; /* Used for lower-right pixel   */

  while (count)   /* Scan image while deletions   */
    {
      pc++;
      count = 0;

      for (i = 0; i < 4; i++)
        {

          m = masks[i];

          /* Build initial previous scan buffer.                  */
          p = (ptr[0] == colour);
          for (x = 0; x < xsize - 1; x++)
            qb[x] = (unsigned char) (p = ((p << 1) & 0006) | (unsigned int) (ptr[x + 1] == colour));

          /* Scan image for pixel deletion candidates.            */
          y_ptr = ptr;
          y1_ptr = ptr + xsize;
          for (y = 0; y < ysize - 1; y++, y_ptr += xsize, y1_ptr += xsize)
            {
              q = qb[0];
              p = ((q << 2) & 0330) | (y1_ptr[0] == colour);

              for (x = 0; x < xsize - 1; x++)
                {
                  q = qb[x];
                  p = ((p << 1) & 0666) | ((q << 3) & 0110) | (unsigned int) (y1_ptr[x + 1] == colour);
                  qb[x] = (unsigned char) p;
                  if (((p & m) == 0) && todelete[p])
                    {
                      count++;
                      y_ptr[x] = bg_color; /* delete the pixel */
                    }
                }

              /* Process right edge pixel.                        */
              p = (p << 1) & 0666;
              if ((p & m) == 0 && todelete[p])
                {
                  count++;
                  y_ptr[xsize - 1] = bg_color;
                }
            }

          /* Process bottom scan line.                            */
          q = qb[0];
          p = ((q << 2) & 0330);

          y_ptr = ptr + xsize * (ysize - 1);
          for (x = 0; x < xsize; x++)
            {
              q = qb[x];
              p = ((p << 1) & 0666) | ((q << 3) & 0110);
              if ((p & m) == 0 && todelete[p])
                {
                  count++;
                  y_ptr[x] = bg_color;
                }
            }
        }
    }
  free(qb);
}

Image thin_image(const Image &box, double THRESHOLD_BOND, const ColorGray &bgColor)
{
  Image image(Geometry(box.columns(), box.rows()), "white");
  image.type(GrayscaleType);
  unsigned int xsize = box.columns();
  unsigned int ysize = box.rows();
  unsigned char *ptr = (unsigned char*) malloc(xsize * ysize * sizeof(unsigned char));

  for (unsigned int i = 0; i < xsize; i++)
    for (unsigned int j = 0; j < ysize; j++)
      ptr[i + j * xsize] = get_pixel(box, bgColor, i, j, THRESHOLD_BOND);

  if (xsize>1 && ysize>1)
    thin1(ptr, xsize, ysize);

  for (unsigned int i = 0; i < xsize; i++)
    for (unsigned int j = 0; j < ysize; j++)
      if (ptr[i + j * xsize] == 1)
        image.pixelColor(i, j, "black");

  free(ptr);
  return (image);
}

int comp_dashes_x(const void *a, const void *b)
{
  dash_t *aa = (dash_t *) a;
  dash_t *bb = (dash_t *) b;
  if (aa->x < bb->x)
    return (-1);
  if (aa->x == bb->x)
    return (0);
  if (aa->x > bb->x)
    return (1);
  return (0);
}

int comp_dashes_y(const void *a, const void *b)
{
  dash_t *aa = (dash_t *) a;
  dash_t *bb = (dash_t *) b;
  if (aa->y < bb->y)
    return (-1);
  if (aa->y == bb->y)
    return (0);
  if (aa->y > bb->y)
    return (1);
  return (0);
}

void extend_dashed_bond(int a, int b, int n, vector<atom_t> &atom)
{
  double x0 = atom[a].x;
  double y0 = atom[a].y;
  double x1 = atom[b].x;
  double y1 = atom[b].y;
  double l = distance(x0, y0, x1, y1);
  double kx = (x1 - x0) / l;
  double ky = (y1 - y0) / l;
  atom[a].x = kx * (-1. * l / (n - 1)) + x0;
  atom[a].y = ky * (-1. * l / (n - 1)) + y0;
  atom[b].x = kx * l / (n - 1) + x1;
  atom[b].y = ky * l / (n - 1) + y1;
}

int count_area(vector<vector<int> > &box, double &x0, double &y0)
{
  int a = 0;
  int w = box.size();
  int h = box[0].size();
  int x = int(x0);
  int y = int(y0);
  int xm = 0, ym = 0;

  if (box[x][y] == 1)
    {
      box[x][y] = 2;
      list<int> cx;
      list<int> cy;
      cx.push_back(x);
      cy.push_back(y);
      while (!cx.empty())
        {
          x = cx.front();
          y = cy.front();
          cx.pop_front();
          cy.pop_front();
          box[x][y] = 0;
          a++;
          xm += x;
          ym += y;
          for (int i = x - 1; i < x + 2; i++)
            for (int j = y - 1; j < y + 2; j++)
              if (i < w && j < h && i >= 0 && j >= 0 && box[i][j] == 1)
                {
                  cx.push_back(i);
                  cy.push_back(j);
                  box[i][j] = 2;
                }
        }
    }
  else
    return (0);

  x0 = 1. * xm / a;
  y0 = 1. * ym / a;

  return (a);
}

int find_dashed_bonds(const potrace_path_t *p, vector<atom_t> &atom, vector<bond_t> &bond, int n_atom, int *n_bond,
                      int max, double avg, const Image &img, const ColorGray &bg, double THRESHOLD, bool thick, double dist)
{
  int n, n_dot = 0;
  potrace_dpoint_t (*c)[3];
  dash_t dot[100];
  vector<vector<int> > box(img.columns());

  for (unsigned int i = 0; i < img.columns(); i++)
    for (unsigned int j = 0; j < img.rows(); j++)
      box[i].push_back(get_pixel(img, bg, i, j, THRESHOLD));

  while (p != NULL)
    {
      if (p->sign == int('+') && p->area < max)
        {
          n = p->curve.n;
          c = p->curve.c;
          int *tag = p->curve.tag;
          dot[n_dot].x = c[n - 1][2].x;
          dot[n_dot].y = c[n - 1][2].y;
          double l = c[n - 1][2].x;
          double r = c[n - 1][2].x;
          double t = c[n - 1][2].y;
          double b = c[n - 1][2].y;
          dot[n_dot].curve = p;
          dot[n_dot].free = true;
          int tot = 1;
          for (long i = 0; i < n; i++)
            {
              switch (tag[i])
                {
                case POTRACE_CORNER:
                  dot[n_dot].x += c[i][1].x;
                  dot[n_dot].y += c[i][1].y;
                  if (c[i][1].x < l)
                    l = c[i][1].x;
                  if (c[i][1].x > r)
                    r = c[i][1].x;
                  if (c[i][1].y < t)
                    t = c[i][1].y;
                  if (c[i][1].x > b)
                    b = c[i][1].y;
                  tot++;
                  break;
                case POTRACE_CURVETO:
                  dot[n_dot].x += c[i][0].x;
                  dot[n_dot].y += c[i][0].y;
                  if (c[i][0].x < l)
                    l = c[i][0].x;
                  if (c[i][0].x > r)
                    r = c[i][0].x;
                  if (c[i][0].y < t)
                    t = c[i][0].y;
                  if (c[i][0].x > b)
                    b = c[i][0].y;
                  dot[n_dot].x += c[i][1].x;
                  dot[n_dot].y += c[i][1].y;
                  if (c[i][1].x < l)
                    l = c[i][1].x;
                  if (c[i][1].x > r)
                    r = c[i][1].x;
                  if (c[i][1].y < t)
                    t = c[i][1].y;
                  if (c[i][1].x > b)
                    b = c[i][1].y;
                  tot += 2;
                  break;
                }
              if (i != n - 1)
                {
                  dot[n_dot].x += c[i][2].x;
                  dot[n_dot].y += c[i][2].y;
                  if (c[i][2].x < l)
                    l = c[i][2].x;
                  if (c[i][2].x > r)
                    r = c[i][2].x;
                  if (c[i][2].y < t)
                    t = c[i][2].y;
                  if (c[i][2].x > b)
                    b = c[i][2].y;
                  tot++;
                }
            }
          dot[n_dot].x /= tot;
          dot[n_dot].y /= tot;
          if (thick)
            dot[n_dot].area = count_area(box, dot[n_dot].x, dot[n_dot].y);
          else
            dot[n_dot].area = p->area;
          if (distance(l, t, r, b) < avg / 3)
            n_dot++;
          if (n_dot >= 100)
            n_dot--;
        }
      p = p->next;
    }
  for (int i = 0; i < n_dot; i++)
    if (dot[i].free)
      {
        dash_t dash[100];
        dash[0] = dot[i];
        dot[i].free = false;
        double l = dot[i].x;
        double r = dot[i].x;
        double t = dot[i].y;
        double b = dot[i].y;
        double mx = l;
        double my = t;
        double dist_next = FLT_MAX;
        int next_dot = i;
        for (int j = i + 1; j < n_dot; j++)
          if (dot[j].free && distance(dash[0].x, dash[0].y, dot[j].x, dot[j].y) <= dist && distance(dash[0].x,
              dash[0].y, dot[j].x, dot[j].y) < dist_next)
            {
              dash[1] = dot[j];
              dist_next = distance(dash[0].x, dash[0].y, dot[j].x, dot[j].y);
              next_dot = j;
            }

        int n = 1;
        if (next_dot != i)
          {
            dot[next_dot].free = false;
            if (dash[1].x < l)
              l = dash[1].x;
            if (dash[1].x > r)
              r = dash[1].x;
            if (dash[1].y < t)
              t = dash[1].y;
            if (dash[1].y > b)
              b = dash[1].y;
            mx = (mx + dash[1].x) / 2;
            my = (my + dash[1].y) / 2;
            n = 2;
          }
        bool found = true;
        while (n > 1 && found)
          {
            dist_next = FLT_MAX;
            found = false;
            int minj = next_dot;
            for (int j = next_dot + 1; j < n_dot; j++)
              if (dot[j].free && distance(mx, my, dot[j].x, dot[j].y) <= dist && distance(mx, my, dot[j].x,
                  dot[j].y) < dist_next
                  //&& fabs(angle4(dash[0].x, dash[0].y, dash[n - 1].x, dash[n - 1].y, dash[0].x, dash[0].y,
                  //		dot[j].x, dot[j].y)) > D_T_TOLERANCE)
                  && fabs(distance_from_bond_y(dash[0].x, dash[0].y, dash[n - 1].x, dash[n - 1].y, dot[j].x,
                                               dot[j].y)) < V_DISPLACEMENT)
                {
                  dash[n] = dot[j];
                  dist_next = distance(mx, my, dot[j].x, dot[j].y);
                  found = true;
                  minj = j;
                }
            if (found)
              {
                dot[minj].free = false;
                if (dash[n].x < l)
                  l = dash[n].x;
                if (dash[n].x > r)
                  r = dash[n].x;
                if (dash[n].y < t)
                  t = dash[n].y;
                if (dash[n].y > b)
                  b = dash[n].y;
                mx = (mx + dash[n].x) / 2;
                my = (my + dash[n].y) / 2;
                n++;
              }
          }

        if (n > 2)
          {
            if ((r - l) > (b - t))
              {
                qsort(dash, n, sizeof(dash_t), comp_dashes_x);
              }
            else
              {
                qsort(dash, n, sizeof(dash_t), comp_dashes_y);
              }
            bool one_line = true;
            double dx = dash[n - 1].x - dash[0].x;
            double dy = dash[n - 1].y - dash[0].y;
            double k = 0;
            if (fabs(dx) > fabs(dy))
              k = dy / dx;
            else
              k = dx / dy;
            for (int j = 1; j < n - 1; j++)
              {
                double nx = dash[j].x - dash[0].x;
                double ny = dash[j].y - dash[0].y;
                double diff = 0;
                if (fabs(dx) > fabs(dy))
                  diff = k * nx - ny;
                else
                  diff = k * ny - nx;
                if (fabs(diff) > V_DISPLACEMENT)
                  one_line = false;
              }
            if (one_line)
              {
                for (int j = 0; j < n; j++)
                  delete_curve(atom, bond, n_atom, *n_bond, dash[j].curve);
                atom_t a1;
                atom.push_back(a1);
                atom[n_atom].x = dash[0].x;
                atom[n_atom].y = dash[0].y;
                atom[n_atom].label = " ";
                atom[n_atom].exists = true;
                atom[n_atom].curve = dash[0].curve;
                atom[n_atom].n = 0;
                atom[n_atom].corner = false;
                atom[n_atom].terminal = false;
                atom[n_atom].charge = 0;
                atom[n_atom].anum = 0;
                n_atom++;
                if (n_atom >= MAX_ATOMS)
                  n_atom--;
                atom_t a2;
                atom.push_back(a2);
                atom[n_atom].x = dash[n - 1].x;
                atom[n_atom].y = dash[n - 1].y;
                atom[n_atom].label = " ";
                atom[n_atom].exists = true;
                atom[n_atom].curve = dash[n - 1].curve;
                atom[n_atom].n = 0;
                atom[n_atom].corner = false;
                atom[n_atom].terminal = false;
                atom[n_atom].charge = 0;
                atom[n_atom].anum = 0;
                n_atom++;
                if (n_atom >= MAX_ATOMS)
                  n_atom--;
                bond_t b1;
                bond.push_back(b1);
                bond[*n_bond].a = n_atom - 2;
                bond[*n_bond].exists = true;
                bond[*n_bond].type = 1;
                bond[*n_bond].b = n_atom - 1;
                bond[*n_bond].curve = dash[0].curve;
                if (dash[0].area > dash[n - 1].area)
                  bond_end_swap(bond, *n_bond);
                bond[*n_bond].hash = true;
                bond[*n_bond].wedge = false;
                bond[*n_bond].up = false;
                bond[*n_bond].down = false;
                bond[*n_bond].Small = false;
                bond[*n_bond].arom = false;
                bond[*n_bond].conjoined = false;
                extend_dashed_bond(bond[*n_bond].a, bond[*n_bond].b, n, atom);
                (*n_bond)++;
                if ((*n_bond) >= MAX_ATOMS)
                  (*n_bond)--;
              }
          }
      }

  return (n_atom);
}

int find_small_bonds(const potrace_path_t *p, vector<atom_t> &atom, vector<bond_t> &bond, int n_atom, int *n_bond,
                     double max_area, double Small, double thickness)
{
  while (p != NULL)
    {
      if ((p->sign == int('+')) && (p->area <= max_area))
        {
          int n_dot = 0;
          dash_t dot[20];
          for (int i = 0; i < n_atom; i++)
            if ((atom[i].exists) && (atom[i].curve == p) && (n_dot < 20))
              {
                dot[n_dot].x = atom[i].x;
                dot[n_dot].y = atom[i].y;
                dot[n_dot].curve = p;
                dot[n_dot].free = true;
                n_dot++;
                if (n_dot >= 20)
                  n_dot--;
              }

          if ((n_dot > 2))
            {
              double l = dot[0].x;
              double r = dot[0].x;
              double t = dot[0].y;
              double b = dot[0].y;
              for (int i = 1; i < n_dot; i++)
                {
                  if (dot[i].x < l)
                    l = dot[i].x;
                  if (dot[i].x > r)
                    r = dot[i].x;
                  if (dot[i].y < t)
                    t = dot[i].y;
                  if (dot[i].y > b)
                    b = dot[i].y;
                }
              if ((r - l) > (b - t))
                {
                  qsort(dot, n_dot, sizeof(dash_t), comp_dashes_x);
                }
              else
                {
                  qsort(dot, n_dot, sizeof(dash_t), comp_dashes_y);
                }
              double d = 0;
              for (int i = 1; i < n_dot - 1; i++)
                d = max(d, fabs(distance_from_bond_y(dot[0].x, dot[0].y, dot[n_dot - 1].x, dot[n_dot - 1].y,
                                                     dot[i].x, dot[i].y)));
              if (d < thickness || p->area < Small)
                {
                  delete_curve(atom, bond, n_atom, *n_bond, p);
                  atom_t a1;
                  atom.push_back(a1);
                  atom[n_atom].x = dot[0].x;
                  atom[n_atom].y = dot[0].y;
                  atom[n_atom].label = " ";
                  atom[n_atom].exists = true;
                  atom[n_atom].curve = p;
                  atom[n_atom].n = 0;
                  atom[n_atom].corner = false;
                  atom[n_atom].terminal = false;
                  atom[n_atom].charge = 0;
                  atom[n_atom].anum = 0;
                  n_atom++;
                  if (n_atom >= MAX_ATOMS)
                    n_atom--;
                  atom_t a2;
                  atom.push_back(a2);
                  atom[n_atom].x = dot[n_dot - 1].x;
                  atom[n_atom].y = dot[n_dot - 1].y;
                  atom[n_atom].label = " ";
                  atom[n_atom].exists = true;
                  atom[n_atom].curve = p;
                  atom[n_atom].n = 0;
                  atom[n_atom].corner = false;
                  atom[n_atom].terminal = false;
                  atom[n_atom].charge = 0;
                  atom[n_atom].anum = 0;
                  n_atom++;
                  if (n_atom >= MAX_ATOMS)
                    n_atom--;
                  bond_t b1;
                  bond.push_back(b1);
                  bond[*n_bond].a = n_atom - 2;
                  bond[*n_bond].exists = true;
                  bond[*n_bond].type = 1;
                  bond[*n_bond].b = n_atom - 1;
                  bond[*n_bond].curve = p;
                  bond[*n_bond].hash = false;
                  bond[*n_bond].wedge = false;
                  bond[*n_bond].up = false;
                  bond[*n_bond].down = false;
                  bond[*n_bond].Small = true;
                  bond[*n_bond].arom = false;
                  bond[*n_bond].conjoined = false;
                  (*n_bond)++;
                  if ((*n_bond) >= MAX_ATOMS)
                    (*n_bond)--;
                }
            }
        }
      p = p->next;
    }
  return (n_atom);
}

int resolve_bridge_bonds(vector<atom_t> &atom, int n_atom, vector<bond_t> &bond, int n_bond, double thickness,
                         double avg_bond_length, const map<string, string> &superatom)
{
  molecule_statistics_t molecule_statistics1 = caclulate_molecule_statistics(atom, bond, n_bond, avg_bond_length, superatom);

  for (int i = 0; i < n_atom; i++)
    if ((atom[i].exists) && (atom[i].label == " "))
      {
        list<int> con;
        for (int j = 0; j < n_bond; j++)
          if ((bond[j].exists) && (bond[j].a == i || bond[j].b == i))
            con.push_back(j);
        if (con.size() == 4)
          {
            int a = con.front();
            con.pop_front();
            int b = 0;
            int e = 0;
            while ((con.size() > 2) && (e++ < 3))
              {
                b = con.front();
                con.pop_front();
                double y1 = distance_from_bond_y(atom[bond[a].a].x, atom[bond[a].a].y, atom[bond[a].b].x,
                                                 atom[bond[a].b].y, atom[bond[b].a].x, atom[bond[b].a].y);
                double y2 = distance_from_bond_y(atom[bond[a].a].x, atom[bond[a].a].y, atom[bond[a].b].x,
                                                 atom[bond[a].b].y, atom[bond[b].b].x, atom[bond[b].b].y);
                if (fabs(y1) > thickness || fabs(y2) > thickness)
                  con.push_back(b);
              }
            if (con.size() == 2)
              {
                int c = con.front();
                con.pop_front();
                int d = con.front();
                con.pop_front();
                vector<int> term;
                term.push_back(a);
                term.push_back(b);
                term.push_back(c);
                term.push_back(d);
                bool terminal = false;
                for (unsigned int k = 0; k < term.size(); k++)
                  {
                    bool terminal_a = terminal_bond(bond[term[k]].a, term[k], bond, n_bond);
                    bool terminal_b = terminal_bond(bond[term[k]].b, term[k], bond, n_bond);
                    if (terminal_a || terminal_b)
                      terminal = true;
                  }
                double y1 = distance_from_bond_y(atom[bond[c].a].x, atom[bond[c].a].y, atom[bond[c].b].x,
                                                 atom[bond[c].b].y, atom[bond[d].a].x, atom[bond[d].a].y);
                double y2 = distance_from_bond_y(atom[bond[c].a].x, atom[bond[c].a].y, atom[bond[c].b].x,
                                                 atom[bond[c].b].y, atom[bond[d].b].x, atom[bond[d].b].y);
                if (bond[a].type == 1 && bond[b].type == 1 && bond[c].type == 1 && bond[d].type == 1 && fabs(y1)
                    < thickness && fabs(y2) < thickness && !terminal)
                  {
                    bond[b].exists = false;
                    bond[d].exists = false;
                    atom[i].exists = false;
                    if (bond[a].a == bond[b].a)
                      bond[a].a = bond[b].b;
                    else if (bond[a].a == bond[b].b)
                      bond[a].a = bond[b].a;
                    else if (bond[a].b == bond[b].a)
                      bond[a].b = bond[b].b;
                    else if (bond[a].b == bond[b].b)
                      bond[a].b = bond[b].a;
                    if (bond[c].a == bond[d].a)
                      bond[c].a = bond[d].b;
                    else if (bond[c].a == bond[d].b)
                      bond[c].a = bond[d].a;
                    else if (bond[c].b == bond[d].a)
                      bond[c].b = bond[d].b;
                    else if (bond[c].b == bond[d].b)
                      bond[c].b = bond[d].a;

                    molecule_statistics_t molecule_statistics2 = caclulate_molecule_statistics(atom, bond, n_bond, avg_bond_length, superatom);
                    if (molecule_statistics1.fragments != molecule_statistics2.fragments ||
                        molecule_statistics1.rotors != molecule_statistics2.rotors ||
                        molecule_statistics1.rings56 - molecule_statistics2.rings56 == 2)
                      {
                        bond[b].exists = true;
                        bond[d].exists = true;
                        atom[i].exists = true;
                        if (bond[a].a == bond[b].a)
                          bond[a].a = bond[b].b;
                        else if (bond[a].a == bond[b].b)
                          bond[a].a = bond[b].a;
                        else if (bond[a].b == bond[b].a)
                          bond[a].b = bond[b].b;
                        else if (bond[a].b == bond[b].b)
                          bond[a].b = bond[b].a;
                        if (bond[c].a == bond[d].a)
                          bond[c].a = bond[d].b;
                        else if (bond[c].a == bond[d].b)
                          bond[c].a = bond[d].a;
                        else if (bond[c].b == bond[d].a)
                          bond[c].b = bond[d].b;
                        else if (bond[c].b == bond[d].b)
                          bond[c].b = bond[d].a;
                      }
                  }
              }
          }
      }
  return (molecule_statistics1.fragments);
}

void collapse_atoms(vector<atom_t> &atom, vector<bond_t> &bond, int n_atom, int n_bond, double dist)
{
  bool found = true;

  while (found)
    {
      found = false;
      for (int i = 0; i < n_atom; i++)
        if (atom[i].exists)
          for (int j = 0; j < n_atom; j++)
            if (atom[j].exists && j != i && distance(atom[i].x, atom[i].y, atom[j].x, atom[j].y) < dist)
              {
                atom[j].exists = false;
                atom[i].x = (atom[i].x + atom[j].x) / 2;
                atom[i].y = (atom[i].y + atom[j].y) / 2;
                if (atom[j].label != " " && atom[i].label == " ")
                  atom[i].label = atom[j].label;
                for (int k = 0; k < n_bond; k++)
                  if (bond[k].exists)
                    {
                      if (bond[k].a == j)
                        {
                          bond[k].a = i;
                        }
                      else if (bond[k].b == j)
                        {
                          bond[k].b = i;
                        }
                    }
                found = true;
              }
    }
}

void collapse_bonds(vector<atom_t> &atom, const vector<bond_t> &bond, int n_bond, double dist)
{
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && bond_length(bond, i, atom) < dist)
      {
        atom[bond[i].a].x = (atom[bond[i].a].x + atom[bond[i].b].x) / 2;
        atom[bond[i].a].y = (atom[bond[i].a].y + atom[bond[i].b].y) / 2;
        atom[bond[i].b].x = (atom[bond[i].a].x + atom[bond[i].b].x) / 2;
        atom[bond[i].b].y = (atom[bond[i].a].y + atom[bond[i].b].y) / 2;
      }
}

int fix_one_sided_bonds(vector<bond_t> &bond, int n_bond, const vector<atom_t> &atom, double thickness, double avg)
{
  double l;

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && bond[i].type < 3 && (l = bond_length(bond, i, atom)) > avg / 3)
      for (int j = 0; j < n_bond; j++)
        if (bond[j].exists && j != i && bond[j].type < 3 && fabs(angle_between_bonds(bond, i, j, atom))
            < D_T_TOLERANCE && bond_length(bond, j, atom) > avg / 3)
          {
            double d1 = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                  atom[bond[i].b].y, atom[bond[j].a].x, atom[bond[j].a].y));
            double d2 = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                  atom[bond[i].b].y, atom[bond[j].b].x, atom[bond[j].b].y));
            if (d1 < thickness && !(bond[j].a == bond[i].b || bond[j].a == bond[i].a))
              {
                double l1 = distance_from_bond_x_a(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                   atom[bond[i].b].y, atom[bond[j].a].x, atom[bond[j].a].y);
                if (l1 > 0 && l1 < l)
                  {
                    if (bond[j].b == bond[i].b || bond[j].b == bond[i].a)
                      {
                        bond[j].exists = false;
                      }
                    else
                      {
                        bond_t b1;
                        bond.push_back(b1);
                        bond[n_bond].b = bond[i].b;
                        bond[n_bond].exists = true;
                        bond[n_bond].type = bond[i].type;
                        bond[n_bond].a = bond[j].a;
                        bond[n_bond].curve = bond[i].curve;
                        if (bond[i].hash)
                          bond[n_bond].hash = true;
                        else
                          bond[n_bond].hash = false;
                        if (bond[i].wedge)
                          bond[n_bond].wedge = true;
                        else
                          bond[n_bond].wedge = false;
                        bond[n_bond].Small = false;
                        bond[n_bond].up = false;
                        bond[n_bond].down = false;
                        if (bond[i].arom)
                          bond[n_bond].arom = true;
                        else
                          bond[n_bond].arom = false;
                        bond[n_bond].conjoined = bond[i].conjoined;
                        n_bond++;
                        if (n_bond >= MAX_ATOMS)
                          n_bond--;
                        bond[i].b = bond[j].a;
                        bond[i].wedge = false;
                      }
                  }
              }
            else if (d2 < thickness && !(bond[j].b == bond[i].b || bond[j].b == bond[i].a))
              {
                double l1 = distance_from_bond_x_a(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                   atom[bond[i].b].y, atom[bond[j].b].x, atom[bond[j].b].y);
                if (l1 > 0 && l1 < l)
                  {
                    if (bond[j].a == bond[i].b || bond[j].a == bond[i].a)
                      {
                        bond[j].exists = false;
                      }
                    else
                      {
                        bond_t b1;
                        bond.push_back(b1);
                        bond[n_bond].b = bond[i].b;
                        bond[n_bond].exists = true;
                        bond[n_bond].type = bond[i].type;
                        bond[n_bond].a = bond[j].b;
                        bond[n_bond].curve = bond[i].curve;
                        if (bond[i].hash)
                          bond[n_bond].hash = true;
                        else
                          bond[n_bond].hash = false;
                        if (bond[i].wedge)
                          bond[n_bond].wedge = true;
                        else
                          bond[n_bond].wedge = false;
                        bond[n_bond].Small = false;
                        bond[n_bond].up = false;
                        bond[n_bond].down = false;
                        if (bond[i].arom)
                          bond[n_bond].arom = true;
                        else
                          bond[n_bond].arom = false;
                        bond[n_bond].conjoined = bond[i].conjoined;
                        n_bond++;
                        if (n_bond >= MAX_ATOMS)
                          n_bond--;
                        bond[i].b = bond[j].b;
                        bond[i].wedge = false;
                      }
                  }
              }
          }

  return (n_bond);
}

int find_fused_chars(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, vector<letters_t> &letters, int n_letters,
                     int max_font_height, int max_font_width, char dummy, const Image &orig, const ColorGray &bgColor,
                     double THRESHOLD, unsigned int size, bool verbose)
{
  double dist = max(max_font_width, max_font_height);

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && bond_length(bond, i, atom) < dist)
      {
        list<int> t;
        t.push_back(i);
        double xmin1 = min(atom[bond[i].a].x, atom[bond[i].b].x);
        double xmax1 = max(atom[bond[i].a].x, atom[bond[i].b].x);
        double ymin1 = min(atom[bond[i].a].y, atom[bond[i].b].y);
        double ymax1 = max(atom[bond[i].a].y, atom[bond[i].b].y);
        for (int j = 0; j < n_bond; j++)
          if (bond[j].exists && bond_length(bond, j, atom) < dist && j != i && atom[bond[j].a].x >= xmin1
              && atom[bond[j].a].x >= xmin1)
            {
              double xmax2 = max(xmax1, max(atom[bond[j].a].x, atom[bond[j].b].x));
              double ymin2 = min(ymin1, min(atom[bond[j].a].y, atom[bond[j].b].y));
              double ymax2 = max(ymax1, max(atom[bond[j].a].y, atom[bond[j].b].y));

              if (xmax2 - xmin1 <= max_font_width && ymax2 - ymin2 <= max_font_height)
                t.push_back(j);
            }

        vector<int> all_bonds(n_bond, 0);
        for (int j = 0; j < n_bond; j++)
          if (bond[j].exists)
            all_bonds[j] = 1;

        list<int> bag1, bag2;
        all_bonds[i] = 2;
        bag1.push_back(i);
        while (!bag1.empty())
          {
            int k = bag1.front();
            bag1.pop_front();
            all_bonds[k] = 3;
            for (int j = 0; j < n_bond; j++)
              if (j != k && all_bonds[j] == 1 && (bond[k].a == bond[j].a || bond[k].a == bond[j].b || bond[k].b
                                                  == bond[j].a || bond[k].b == bond[j].b))
                {
                  all_bonds[j] = 2;
                  bag1.push_back(j);
                }
          }
        while (!t.empty())
          {
            int k = t.front();
            t.pop_front();
            if (all_bonds[k] == 3)
              bag2.push_back(k);
          }

        unsigned int bag_size = bag2.size();
        if (bag_size > size)
          {
            double cx = 0;
            double cy = 0;
            int n = 0;
            double l = FLT_MAX, r = 0, t = FLT_MAX, b = 0;

            while (!bag2.empty())
              {
                int k = bag2.front();
                bag2.pop_front();
                cx += atom[bond[k].a].x + atom[bond[k].b].x;
                cy += atom[bond[k].a].y + atom[bond[k].b].y;
                l = min(l, min(atom[bond[k].a].x, atom[bond[k].b].x));
                r = max(r, max(atom[bond[k].a].x, atom[bond[k].b].x));
                t = min(t, min(atom[bond[k].a].y, atom[bond[k].b].y));
                b = max(b, max(atom[bond[k].a].y, atom[bond[k].b].y));
                n += 2;
              }
            cx /= n;
            cy /= n;
            if (r - l > MIN_FONT_HEIGHT && b - t > MIN_FONT_HEIGHT)
              {
                int left = int(cx - max_font_width / 2) - 1;
                int right = int(cx + max_font_width / 2) - 1;
                int top = int(cy - max_font_height / 2);
                int bottom = int(cy + max_font_height / 2);
                char label = 0;
                if (dummy != 0)
                  {

                    label = dummy;
                    //cout << bag_size << " " << left << " " << top << " " << right << " " << bottom << endl;
                  }
                else
                  {
                    label = get_atom_label(orig, bgColor, left, top, right, bottom, THRESHOLD, (left + right) / 2,
                                           top, verbose);
                  }
                if ((label != 0 && label != 'P' && label != 'p' && label != 'F' && label != 'X' && label != 'Y'
                     && label != 'n' && label != 'F' && label != 'U' && label != 'u' && label != 'h') || dummy
                    != 0)
                  {

                    bool overlap = false;
                    for (int j = 0; j < n_letters; j++)
                      {
                        if (distance((left + right) / 2, (top + bottom) / 2, letters[j].x, letters[j].y)
                            < letters[j].r)
                          overlap = true;
                      }
                    if (!overlap)
                      {
                        letters_t lt;
                        letters.push_back(lt);
                        letters[n_letters].a = label;
                        letters[n_letters].x = (left + right) / 2;
                        letters[n_letters].y = (top + bottom) / 2;
                        letters[n_letters].r = distance(left, top, right, bottom) / 2;
                        letters[n_letters].free = true;
                        n_letters++;
                        if (n_letters >= MAX_ATOMS)
                          n_letters--;
                      }
                    delete_bonds_in_char(bond, n_bond, atom, left, top, right, bottom);
                  }

              }
          }
      }
  return (n_letters);
}

bool comp_boxes(const box_t &aa, const box_t &bb)
{
  if (aa.y2 < bb.y1)
    return (true);
  if (aa.y1 > bb.y2)
    return (false);
  if (aa.x1 > bb.x1)
    return (false);
  if (aa.x1 < bb.x1)
    return (true);
  return (false);
}

double noise_factor(const Image &image, int width, int height, const ColorGray &bgColor, double THRESHOLD_BOND,
                    int resolution, int &max, double &nf45)
{
  int max_thick = 40;
  vector<double> n(max_thick, 0);
  double nf;

  for (int i = 0; i < width; i++)
    {
      int j = 0;
      while (j < height)
        {
          while (!get_pixel(image, bgColor, i, j, THRESHOLD_BOND) && j < height)
            j++;
          int l = 0;
          while (get_pixel(image, bgColor, i, j, THRESHOLD_BOND) && j < height)
            {
              l++;
              j++;
            }
          if (l < max_thick)
            n[l]++;
        }
    }
  for (int i = 0; i < height; i++)
    {
      int j = 0;
      while (j < width)
        {
          while (!get_pixel(image, bgColor, j, i, THRESHOLD_BOND) && j < width)
            j++;
          int l = 0;
          while (get_pixel(image, bgColor, j, i, THRESHOLD_BOND) && j < width)
            {
              l++;
              j++;
            }
          if (l < max_thick)
            n[l]++;
        }
    }
  double max_v = 0;
  max = 1;
  for (int l = 1; l < max_thick; l++)
    {
      //cout << l << " " << n[l] << endl;
      if (n[l] > max_v)
        {
          max_v = n[l];
          max = l;
        }
    }
  if (max > 2)
    nf = n[2] / n[3];
  else if (max == 2)
    nf = n[1] / n[2];
  else
    nf = n[2] / n[1];
  if (n[5]!=0) nf45=n[4]/n[5];
  else nf45=0;
  return (nf);
}

int thickness_hor(const Image &image, int x1, int y1, const ColorGray &bgColor, double THRESHOLD_BOND)
{
  int i = 0, s = 0, w = 0;
  int width = image.columns();
  s = get_pixel(image, bgColor, x1, y1, THRESHOLD_BOND);

  if (s == 0 && x1 + 1 < width)
    {
      x1++;
      s = get_pixel(image, bgColor, x1, y1, THRESHOLD_BOND);
    }
  if (s == 0 && x1 - 2 >= 0)
    {
      x1 -= 2;
      s = get_pixel(image, bgColor, x1, y1, THRESHOLD_BOND);
    }
  if (s == 1)
    {
      while (x1 + i < width && s == 1)
        s = get_pixel(image, bgColor, x1 + i++, y1, THRESHOLD_BOND);
      w = i - 1;
      i = 1;
      s = 1;
      while (x1 - i >= 0 && s == 1)
        s = get_pixel(image, bgColor, x1 - i++, y1, THRESHOLD_BOND);
      w += i - 1;
    }
  return (w);
}

int thickness_ver(const Image &image, int x1, int y1, const ColorGray &bgColor, double THRESHOLD_BOND)
{
  int i = 0, s = 0, w = 0;
  int height = image.rows();
  s = get_pixel(image, bgColor, x1, y1, THRESHOLD_BOND);

  if (s == 0 && y1 + 1 < height)
    {
      y1++;
      s = get_pixel(image, bgColor, x1, y1, THRESHOLD_BOND);
    }
  if (s == 0 && y1 - 2 >= 0)
    {
      y1 -= 2;
      s = get_pixel(image, bgColor, x1, y1, THRESHOLD_BOND);
    }
  if (s == 1)
    {
      while (y1 + i < height && s == 1)
        s = get_pixel(image, bgColor, x1, y1 + i++, THRESHOLD_BOND);
      w = i - 1;
      i = 1;
      s = 1;
      while (y1 - i >= 0 && s == 1)
        s = get_pixel(image, bgColor, x1, y1 - i++, THRESHOLD_BOND);
      w += i - 1;
    }
  return (w);
}

double find_wedge_bonds(const Image &image, vector<atom_t> &atom, int n_atom, vector<bond_t> &bond, int n_bond,
                        const ColorGray &bgColor, double THRESHOLD_BOND, double max_dist_double_bond, double avg, int limit, int dist =
                          0)
{
  double l;
  vector<double> a;
  int n = 0;
  a.push_back(1.5);
  vector<int> x_reg, y_reg;

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && !bond[i].hash && bond[i].type == 1 && (l = bond_length(bond, i, atom))
        > max_dist_double_bond)
      {
        x_reg.clear();
        y_reg.clear();
        double avg_x = 0, avg_y = 0;
        int x1 = int((atom[bond[i].a].x + atom[bond[i].b].x) / 2);
        int y1 = int((atom[bond[i].a].y + atom[bond[i].b].y) / 2);

        int w = 0, max_c, min_c, sign = 1;
        int w3_ver = thickness_ver(image, x1, y1, bgColor, THRESHOLD_BOND);
        int w3_hor = thickness_hor(image, x1, y1, bgColor, THRESHOLD_BOND);
        if (w3_ver == 0 && w3_hor == 0)
          continue;
        if ((w3_ver < w3_hor && w3_ver > 0) || w3_hor == 0)
          {
            w = w3_ver;
            int old = w3_ver;
            max_c = int(max(atom[bond[i].a].x, atom[bond[i].b].x)) - dist;
            min_c = int(min(atom[bond[i].a].x, atom[bond[i].b].x)) + dist;
            if (atom[bond[i].b].x < atom[bond[i].a].x)
              sign = -1;
            for (int j = x1 + 1; j <= max_c; j++)
              {
                int y = int(atom[bond[i].a].y + (atom[bond[i].b].y - atom[bond[i].a].y) * (j - atom[bond[i].a].x)
                            / (atom[bond[i].b].x - atom[bond[i].a].x));
                int t = thickness_ver(image, j, y, bgColor, THRESHOLD_BOND);
                if (abs(t - old) > 2)
                  break;
                if (t < 2 * MAX_BOND_THICKNESS && t < avg / 3 && t > 0)
                  {
                    x_reg.push_back(j);
                    y_reg.push_back(t);
                    avg_x += j;
                    avg_y += t;
                    w = max(w, t);
                  }
                old = t;
              }
            old = w3_ver;
            for (int j = x1 - 1; j >= min_c; j--)
              {
                int y = int(atom[bond[i].a].y + (atom[bond[i].b].y - atom[bond[i].a].y) * (j - atom[bond[i].a].x)
                            / (atom[bond[i].b].x - atom[bond[i].a].x));
                int t = thickness_ver(image, j, y, bgColor, THRESHOLD_BOND);
                if (abs(t - old) > 2)
                  break;
                if (t < 2 * MAX_BOND_THICKNESS && t < avg / 3 && t > 0)
                  {
                    x_reg.push_back(j);
                    y_reg.push_back(t);
                    avg_x += j;
                    avg_y += t;
                    w = max(w, t);
                  }
                old = t;
              }

          }
        else
          {
            w = w3_hor;
            int old = w3_hor;
            max_c = int(max(atom[bond[i].a].y, atom[bond[i].b].y)) - dist;
            min_c = int(min(atom[bond[i].a].y, atom[bond[i].b].y)) + dist;
            if (atom[bond[i].b].y < atom[bond[i].a].y)
              sign = -1;
            for (int j = y1 + 1; j <= max_c; j++)
              {
                int x = int(atom[bond[i].a].x + (atom[bond[i].b].x - atom[bond[i].a].x) * (j - atom[bond[i].a].y)
                            / (atom[bond[i].b].y - atom[bond[i].a].y));
                int t = thickness_hor(image, x, j, bgColor, THRESHOLD_BOND);
                if (abs(t - old) > 2)
                  break;
                if (t < 2 * MAX_BOND_THICKNESS && t < avg / 3 && t > 0)
                  {
                    x_reg.push_back(j);
                    y_reg.push_back(t);
                    avg_x += j;
                    avg_y += t;
                    w = max(w, t);
                  }
                old = t;
              }
            old = w3_hor;
            for (int j = y1 - 1; j >= min_c; j--)
              {
                int x = int(atom[bond[i].a].x + (atom[bond[i].b].x - atom[bond[i].a].x) * (j - atom[bond[i].a].y)
                            / (atom[bond[i].b].y - atom[bond[i].a].y));
                int t = thickness_hor(image, x, j, bgColor, THRESHOLD_BOND);
                if (abs(t - old) > 2)
                  break;
                if (t < 2 * MAX_BOND_THICKNESS && t < avg / 3 && t > 0)
                  {
                    x_reg.push_back(j);
                    y_reg.push_back(t);
                    avg_x += j;
                    avg_y += t;
                    w = max(w, t);
                  }
                old = t;
              }
          }
        avg_x /= x_reg.size();
        avg_y /= y_reg.size();
        double numerator = 0, denominator = 0;
        for (unsigned int j = 0; j < x_reg.size(); j++)
          {
            numerator += 1. * (x_reg[j] - avg_x) * (y_reg[j] - avg_y);
            denominator += 1. * (x_reg[j] - avg_x) * (x_reg[j] - avg_x);
          }
        double beta = 0;
        if (denominator != 0)
          beta = numerator / denominator;
        //cout << fabs(beta) * (max_c - min_c) << " " << (max_c - min_c) << " " << avg << endl;
        if (fabs(beta) * (max_c - min_c) > limit)
          {
            bond[i].wedge = true;
            if (beta * sign < 0)
              bond_end_swap(bond, i);
          }
        if (bond[i].wedge)
          {
            for (int j = 0; j < n_atom; j++)
              if (atom[j].exists && j != bond[i].b && distance(atom[bond[i].b].x, atom[bond[i].b].y, atom[j].x,
                  atom[j].y) <= w)
                {
                  atom[j].exists = false;
                  atom[bond[i].b].x = (atom[bond[i].b].x + atom[j].x) / 2;
                  atom[bond[i].b].y = (atom[bond[i].b].y + atom[j].y) / 2;
                  for (int k = 0; k < n_bond; k++)
                    if (bond[k].exists)
                      {
                        if (bond[k].a == j)
                          {
                            bond[k].a = bond[i].b;
                          }
                        else if (bond[k].b == j)
                          {
                            bond[k].b = bond[i].b;
                          }
                      }
                }
          }
        if (!bond[i].wedge)
          {
            a.push_back(int(avg_y));
            n++;
          }
      }
  std::sort(a.begin(), a.end());
  double t;
  if (n > 0)
    t = a[(n - 1) / 2];
  else
    t = 1.5;
  //cout << "----------------" << endl;
  return (t);
}

void collapse_double_bonds(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, double dist)
{
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && bond[i].type == 2 && bond[i].conjoined) // uninitialized value here!!!
      for (int j = 0; j < n_bond; j++)
        if (bond[j].exists && j != i && bond[j].type == 1 && bond_length(bond, j, atom) <= dist)
          {
            if (bond[j].a == bond[i].a)
              {
                bond[j].exists = false;
                atom[bond[i].a].x = (atom[bond[i].a].x + atom[bond[j].b].x) / 2;
                atom[bond[i].a].y = (atom[bond[i].a].y + atom[bond[j].b].y) / 2;
                for (int k = 0; k < n_bond; k++)
                  if (bond[k].exists)
                    {
                      if (bond[k].a == bond[j].b)
                        {
                          bond[k].a = bond[i].a;
                        }
                      else if (bond[k].b == bond[j].b)
                        {
                          bond[k].b = bond[i].a;
                        }
                    }
              }
            else if (bond[j].b == bond[i].a)
              {
                bond[j].exists = false;
                atom[bond[i].a].x = (atom[bond[i].a].x + atom[bond[j].a].x) / 2;
                atom[bond[i].a].y = (atom[bond[i].a].y + atom[bond[j].a].y) / 2;
                for (int k = 0; k < n_bond; k++)
                  if (bond[k].exists)
                    {
                      if (bond[k].a == bond[j].a)
                        {
                          bond[k].a = bond[i].a;
                        }
                      else if (bond[k].b == bond[j].a)
                        {
                          bond[k].b = bond[i].a;
                        }
                    }
              }
            else if (bond[j].a == bond[i].b)
              {
                bond[j].exists = false;
                atom[bond[i].b].x = (atom[bond[i].b].x + atom[bond[j].b].x) / 2;
                atom[bond[i].b].y = (atom[bond[i].b].y + atom[bond[j].b].y) / 2;
                for (int k = 0; k < n_bond; k++)
                  if (bond[k].exists)
                    {
                      if (bond[k].a == bond[j].b)
                        {
                          bond[k].a = bond[i].b;
                        }
                      else if (bond[k].b == bond[j].b)
                        {
                          bond[k].b = bond[i].b;
                        }
                    }
              }
            else if (bond[j].b == bond[i].b)
              {
                bond[j].exists = false;
                atom[bond[i].b].x = (atom[bond[i].b].x + atom[bond[j].a].x) / 2;
                atom[bond[i].b].y = (atom[bond[i].b].y + atom[bond[j].a].y) / 2;
                for (int k = 0; k < n_bond; k++)
                  if (bond[k].exists)
                    {
                      if (bond[k].a == bond[j].a)
                        {
                          bond[k].a = bond[i].b;
                        }
                      else if (bond[k].b == bond[j].a)
                        {
                          bond[k].b = bond[i].b;
                        }
                    }
              }
          }
}

void find_up_down_bonds(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, double thickness)
{
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && bond[i].type == 2)
      {
        if (atom[bond[i].a].x > atom[bond[i].b].x)
          bond_end_swap(bond, i);
        if (atom[bond[i].a].x == atom[bond[i].b].x && atom[bond[i].a].y > atom[bond[i].b].y)
          bond_end_swap(bond, i);

        for (int j = 0; j < n_bond; j++)
          if (bond[j].exists && bond[j].type == 1 && !bond[j].wedge && !bond[j].hash)
            {
              bond[j].down = false;
              bond[j].up = false;
              if (bond[j].b == bond[i].a)
                {
                  double h = distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                  atom[bond[i].b].y, atom[bond[j].a].x, atom[bond[j].a].y);
                  if (h > thickness)
                    bond[j].down = true;
                  else if (h < -thickness)
                    bond[j].up = true;
                }
              else if (bond[j].a == bond[i].a)
                {
                  bond_end_swap(bond, j);
                  double h = distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                  atom[bond[i].b].y, atom[bond[j].a].x, atom[bond[j].a].y);
                  if (h > thickness)
                    bond[j].down = true;
                  else if (h < -thickness)
                    bond[j].up = true;
                }
              else if (bond[j].a == bond[i].b)
                {
                  double h = distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                  atom[bond[i].b].y, atom[bond[j].b].x, atom[bond[j].b].y);
                  if (h > thickness)
                    bond[j].up = true;
                  else if (h < -thickness)
                    bond[j].down = true;
                }
              else if (bond[j].b == bond[i].b)
                {
                  bond_end_swap(bond, j);
                  double h = distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                  atom[bond[i].b].y, atom[bond[j].b].x, atom[bond[j].b].y);
                  if (h > thickness)
                    bond[j].up = true;
                  else if (h < -thickness)
                    bond[j].down = true;
                }
            }
      }
}

bool detect_curve(vector<bond_t> &bond, int n_bond, const potrace_path_t * const curve)
{
  bool res = false;
  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && bond[i].curve == curve && bond[i].type == 1 && !bond[i].wedge && !bond[i].hash)
      res = true;
  return (res);
}

int find_plus_minus(const potrace_path_t *p, vector<letters_t> &letters, vector<atom_t> &atom, vector<bond_t> &bond,
                    int n_atom, int n_bond, int height, int width, int max_font_height, int max_font_width, int n_letters)
{
  int n, *tag;
  potrace_dpoint_t (*c)[3];

  while (p != NULL)
    {
      if ((p->sign == int('+')) && detect_curve(bond, n_bond, p))
        {
          n = p->curve.n;
          tag = p->curve.tag;
          c = p->curve.c;
          int top = height;
          int x1 = 0;
          int left = width;
          int y1 = 0;
          int bottom = 0;
          int x2 = 0;
          int right = 0;
          int y2 = 0;
          for (int i = 0; i < n; i++)
            {
              switch (tag[i])
                {
                case POTRACE_CORNER:
                  if (c[i][1].x < left)
                    {
                      left = int(c[i][1].x);
                      y1 = int(c[i][1].y);
                    }
                  if (c[i][1].x > right)
                    {
                      right = int(c[i][1].x);
                      y2 = int(c[i][1].y);
                    }
                  if (c[i][1].y < top)
                    {
                      top = int(c[i][1].y);
                      x1 = int(c[i][1].x);
                    }
                  if (c[i][1].y > bottom)
                    {
                      bottom = int(c[i][1].y);
                      x2 = int(c[i][1].x);
                    }
                  break;
                case POTRACE_CURVETO:
                  if (c[i][0].x < left)
                    {
                      left = int(c[i][0].x);
                      y1 = int(c[i][0].y);
                    }
                  if (c[i][0].x > right)
                    {
                      right = int(c[i][0].x);
                      y2 = int(c[i][0].y);
                    }
                  if (c[i][0].y < top)
                    {
                      top = int(c[i][0].y);
                      x1 = int(c[i][0].x);
                    }
                  if (c[i][0].y > bottom)
                    {
                      bottom = int(c[i][0].y);
                      x2 = int(c[i][0].x);
                    }
                  if (c[i][1].x < left)
                    {
                      left = int(c[i][1].x);
                      y1 = int(c[i][1].y);
                    }
                  if (c[i][1].x > right)
                    {
                      right = int(c[i][1].x);
                      y2 = int(c[i][1].y);
                    }
                  if (c[i][1].y < top)
                    {
                      top = int(c[i][1].y);
                      x1 = int(c[i][1].x);
                    }
                  if (c[i][1].y > bottom)
                    {
                      bottom = int(c[i][1].y);
                      x2 = int(c[i][1].x);
                    }
                  break;
                }
              if (c[i][2].x < left)
                {
                  left = int(c[i][2].x);
                  y1 = int(c[i][2].y);
                }
              if (c[i][2].x > right)
                {
                  right = int(c[i][2].x);
                  y2 = int(c[i][2].y);
                }
              if (c[i][2].y < top)
                {
                  top = int(c[i][2].y);
                  x1 = int(c[i][2].x);
                }
              if (c[i][2].y > bottom)
                {
                  bottom = int(c[i][2].y);
                  x2 = int(c[i][2].x);
                }
            }

          if (((bottom - top) <= max_font_height) && ((right - left) <= max_font_width) && (right - left > 1)
              //&& (right-left)<avg
             )
            {
              double aspect = 1. * (bottom - top) / (right - left);
              double fill = 0;
              if ((bottom - top) * (right - left) != 0)
                fill = 1. * p->area / ((bottom - top) * (right - left));
              else if ((bottom - top) == 0)
                fill = 1.;
              else if ((right - left) == 0)
                fill = 0.;
              char c = ' ';
              bool char_to_right = false;
              bool inside_char = false;
              for (int j = 0; j < n_letters; j++)
                {
                  if (letters[j].x > right && (top + bottom) / 2 > letters[j].y - letters[j].r && (top + bottom) / 2
                      < letters[j].y + letters[j].r && right > letters[j].x - 2 * letters[j].r && letters[j].a
                      != '-' && letters[j].a != '+')
                    char_to_right = true;
                  if (letters[j].x - letters[j].r <= left && letters[j].x + letters[j].r >= right && letters[j].y
                      - letters[j].r <= top && letters[j].y + letters[j].r >= bottom)
                    inside_char = true;
                }
              //cout << left << "," << y1 << " " << right << "," << y2 << " " << top << "," << x1 << " " << bottom
              //		<< "," << x2 << endl;
              //cout << left << " " << y1 << " " << aspect << " " << fill << endl;
              //cout << aspect << " " << abs(y1 - y2) << " " << abs(y1 + y2 - bottom - top) / 2 << " " << abs(x1 - x2)
              //		<< " " << abs(x1 + x2 - right - left) / 2 << endl;
              if (aspect < 0.7 && fill > 0.9 && !char_to_right && !inside_char)
                c = '-';
              else if (aspect > 0.7 && aspect < 1. / 0.7 && abs(y1 - y2) < 3 && abs(y1 + y2 - bottom - top) / 2 < 3
                       && abs(x1 - x2) < 3 && abs(x1 + x2 - right - left) / 2 < 3 && !inside_char
                       //&& !char_to_right
                      )
                c = '+';
              if (c != ' ')
                {
                  letters_t lt;
                  letters.push_back(lt);
                  letters[n_letters].a = c;
                  letters[n_letters].x = (left + right) / 2;
                  letters[n_letters].y = (top + bottom) / 2;
                  letters[n_letters].r = distance(left, top, right, bottom) / 2;
                  letters[n_letters].free = true;
                  n_letters++;
                  if (n_letters >= MAX_ATOMS)
                    n_letters--;
                  delete_curve_with_children(atom, bond, n_atom, n_bond, p);
                }
            }
        }
      p = p->next;
    }
  return (n_letters);
}

void find_old_aromatic_bonds(const potrace_path_t *p, vector<bond_t> &bond, int n_bond, vector<atom_t> &atom,
                             int n_atom, double avg)
{
  const potrace_path_t *p1 = p;

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      bond[i].arom = false;
  while (p != NULL)
    {
      if ((p->sign == int('-')) && detect_curve(bond, n_bond, p))
        {
          potrace_path_t *child = p->childlist;
          if (child != NULL && child->sign == int('+'))
            {
              potrace_path_t *gchild = child->childlist;
              if (gchild != NULL && gchild->sign == int('-'))
                {
                  for (int i = 0; i < n_bond; i++)
                    if (bond[i].exists && bond[i].curve == p)
                      bond[i].arom = true;
                  delete_curve_with_children(atom, bond, n_atom, n_bond, child);
                }
            }
        }
      p = p->next;
    }

  while (p1 != NULL)
    {
      if (p1->sign == int('+') && detect_curve(bond, n_bond, p1))
        {
          potrace_path_t *child = p1->childlist;
          if (child != NULL && child->sign == int('-'))
            {
              vector<int> vert;
              double circum = 0;
              for (int i = 0; i < n_bond; i++)
                if (bond[i].exists && bond[i].curve == p1)
                  circum += bond_length(bond, i, atom);
              for (int i = 0; i < n_atom; i++)
                if (atom[i].exists && atom[i].curve == p1)
                  vert.push_back(i);
              if (vert.size() > 4)
                {
                  double diameter = 0, center_x = 0, center_y = 0;
                  int num = 0;
                  for (unsigned int i = 0; i < vert.size(); i++)
                    {
                      for (unsigned int j = i + 1; j < vert.size(); j++)
                        {
                          double dist = distance(atom[vert[i]].x, atom[vert[i]].y, atom[vert[j]].x, atom[vert[j]].y);
                          if (dist > diameter)
                            diameter = dist;
                        }
                      center_x += atom[vert[i]].x;
                      center_y += atom[vert[i]].y;
                      num++;
                    }
                  center_x /= num;
                  center_y /= num;
                  bool centered = true;
                  for (unsigned int i = 0; i < vert.size(); i++)
                    {
                      double dist = distance(atom[vert[i]].x, atom[vert[i]].y, center_x, center_y);
                      if (fabs(dist - diameter / 2) > V_DISPLACEMENT)
                        centered = false;
                    }

                  if (circum < PI * diameter && diameter > avg / 2 && diameter < 3 * avg && centered)
                    {
                      delete_curve_with_children(atom, bond, n_atom, n_bond, p1);
                      for (int i = 0; i < n_bond; i++)
                        if (bond[i].exists)
                          {
                            double dist = distance((atom[bond[i].a].x + atom[bond[i].b].x) / 2, (atom[bond[i].a].y
                                                   + atom[bond[i].b].y) / 2, center_x, center_y);
                            double ang = angle4(atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[i].a].x,
                                                atom[bond[i].a].y, center_x, center_y, atom[bond[i].a].x, atom[bond[i].a].y);
                            ang = acos(ang) * 180.0 / PI;
                            if (ang < 90 && dist < (avg / 3 + diameter / 2))
                              {
                                bond[i].arom = true;
                              }
                          }
                    }

                }
            }
        }
      p1 = p1->next;
    }

}

void flatten_bonds(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, double maxh)
{
  bool found = true;

  while (found)
    {
      found = false;
      for (int i = 0; i < n_bond; i++)
        if (bond[i].exists && bond[i].type < 3)
          {
            int n = 0;
            int f = i;
            double li = bond_length(bond, i, atom);
            if (atom[bond[i].a].label == " ")
              {
                for (int j = 0; j < n_bond; j++)
                  if (j != i && bond[j].exists && bond[j].type < 3 && (bond[i].a == bond[j].a || bond[i].a
                      == bond[j].b))
                    {
                      n++;
                      f = j;
                    }
                double lf = bond_length(bond, f, atom);
                if (n == 1)
                  {
                    if (bond[i].a == bond[f].b)
                      {
                        double h = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                             atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[f].a].x, atom[bond[f].a].y));
                        double d = distance_from_bond_x_a(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                          atom[bond[i].b].y, atom[bond[f].a].x, atom[bond[f].a].y);
                        if (h <= maxh && d < 0)
                          {
                            bond[f].exists = false;
                            atom[bond[f].b].exists = false;
                            bond[i].a = bond[f].a;
                            if (lf > li)
                              bond[i].type = bond[f].type;
                            if (bond[f].arom)
                              bond[i].arom = true;
                            if (bond[f].hash)
                              bond[i].hash = true;
                            if (bond[f].wedge)
                              bond[i].wedge = true;
                            found = true;
                          }
                      }
                    else
                      {
                        double h = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                             atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[f].b].x, atom[bond[f].b].y));
                        double d = distance_from_bond_x_a(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                          atom[bond[i].b].y, atom[bond[f].b].x, atom[bond[f].b].y);
                        if (h <= maxh && d < 0)
                          {
                            bond[f].exists = false;
                            atom[bond[f].a].exists = false;

                            if (bond[f].hash || bond[f].wedge)
                              {
                                bond[i].a = bond[i].b;
                                bond[i].b = bond[f].b;
                              }
                            else
                              bond[i].a = bond[f].b;
                            if (lf > li)
                              bond[i].type = bond[f].type;
                            if (bond[f].arom)
                              bond[i].arom = true;
                            if (bond[f].hash)
                              bond[i].hash = true;
                            if (bond[f].wedge)
                              bond[i].wedge = true;
                            found = true;
                          }
                      }
                  }
              }

            n = 0;
            f = i;
            if (atom[bond[i].b].label == " ")
              {
                for (int j = 0; j < n_bond; j++)
                  if (j != i && bond[j].exists && bond[j].type < 3 && (bond[i].b == bond[j].a || bond[i].b
                      == bond[j].b))
                    {
                      n++;
                      f = j;
                    }
                double lf = bond_length(bond, f, atom);
                if (n == 1)
                  {
                    if (bond[i].b == bond[f].b)
                      {
                        double h = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                             atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[f].a].x, atom[bond[f].a].y));
                        double d = distance_from_bond_x_b(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                          atom[bond[i].b].y, atom[bond[f].a].x, atom[bond[f].a].y);
                        if (h <= maxh && d > 0)
                          {
                            bond[f].exists = false;
                            atom[bond[f].b].exists = false;
                            if (bond[f].hash || bond[f].wedge)
                              {
                                bond[i].b = bond[i].a;
                                bond[i].a = bond[f].a;
                              }
                            else
                              bond[i].b = bond[f].a;
                            if (lf > li)
                              bond[i].type = bond[f].type;
                            if (bond[f].arom)
                              bond[i].arom = true;
                            if (bond[f].hash)
                              bond[i].hash = true;
                            if (bond[f].wedge)
                              bond[i].wedge = true;
                            found = true;
                          }
                      }
                    else
                      {
                        double h = fabs(distance_from_bond_y(atom[bond[i].a].x, atom[bond[i].a].y,
                                                             atom[bond[i].b].x, atom[bond[i].b].y, atom[bond[f].b].x, atom[bond[f].b].y));
                        double d = distance_from_bond_x_b(atom[bond[i].a].x, atom[bond[i].a].y, atom[bond[i].b].x,
                                                          atom[bond[i].b].y, atom[bond[f].b].x, atom[bond[f].b].y);
                        if (h <= maxh && d > 0)
                          {
                            bond[f].exists = false;
                            atom[bond[f].a].exists = false;
                            bond[i].b = bond[f].b;
                            if (lf > li)
                              bond[i].type = bond[f].type;
                            if (bond[f].arom)
                              bond[i].arom = true;
                            if (bond[f].hash)
                              bond[i].hash = true;
                            if (bond[f].wedge)
                              bond[i].wedge = true;
                            found = true;
                          }
                      }
                  }
              }
          }
    }
}

int clean_unrecognized_characters(vector<bond_t> &bond, int n_bond, const vector<atom_t> &atom, int real_font_height,
                                  int real_font_width, unsigned int size, vector<letters_t> &letters, int n_letters)
{
  vector<int> all_bonds(n_bond, 0);

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists)
      all_bonds[i] = 1;

  for (int i = 0; i < n_bond; i++)
    if (all_bonds[i] == 1)
      {
        list<int> bag1, bag2;
        list<int> trash;
        all_bonds[i] = 2;
        bag1.push_back(i);
        while (!bag1.empty())
          {
            int k = bag1.front();
            bag1.pop_front();
            all_bonds[k] = 0;
            bag2.push_back(k);
            for (int j = 0; j < n_bond; j++)
              if (j != k && all_bonds[j] == 1 && (bond[k].a == bond[j].a || bond[k].a == bond[j].b || bond[k].b
                                                  == bond[j].a || bond[k].b == bond[j].b))
                {
                  all_bonds[j] = 2;
                  bag1.push_back(j);
                }
          }
        double t = FLT_MAX, b = 0, l = FLT_MAX, r = 0;
        while (!bag2.empty())
          {
            int k = bag2.front();
            bag2.pop_front();
            trash.push_back(k);

            if (atom[bond[k].a].x < l)
              l = atom[bond[k].a].x;
            if (atom[bond[k].b].x < l)
              l = atom[bond[k].b].x;
            if (atom[bond[k].a].x > r)
              r = atom[bond[k].a].x;
            if (atom[bond[k].b].x > r)
              r = atom[bond[k].b].x;
            if (atom[bond[k].a].y < t)
              t = atom[bond[k].a].y;
            if (atom[bond[k].b].y < t)
              t = atom[bond[k].b].y;
            if (atom[bond[k].a].y > b)
              b = atom[bond[k].a].y;
            if (atom[bond[k].b].y > b)
              b = atom[bond[k].b].y;
          }
        if ((r - l) < real_font_width && (b - t) < real_font_height && trash.size() > size)
          {
            while (!trash.empty())
              {
                int k = trash.front();
                trash.pop_front();
                bond[k].exists = false;
              }
            letters_t lt;
            letters.push_back(lt);
            letters[n_letters].a = '*';
            letters[n_letters].x = (l + r) / 2;
            letters[n_letters].y = (t + b) / 2;
            letters[n_letters].r = distance(l, t, r, b) / 2;
            letters[n_letters].free = true;
            n_letters++;
            if (n_letters >= MAX_ATOMS)
              n_letters--;
          }
      }
  return (n_letters);
}

void remove_small_terminal_bonds(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, double avg)
{
  bool found = true;

  while (found)
    {
      found = false;
      for (int j = 0; j < n_bond; j++)
        if (bond[j].exists && bond[j].type == 1 && !bond[j].wedge && !bond[j].hash && !bond[j].arom && bond_length(
              bond, j, atom) < avg / 3)
          {
            bool not_corner_a = terminal_bond(bond[j].a, j, bond, n_bond);
            bool not_corner_b = terminal_bond(bond[j].b, j, bond, n_bond);
            if (not_corner_a)
              {
                bond[j].exists = false;
                atom[bond[j].a].exists = false;
                found = true;
                if (atom[bond[j].b].label == " ")
                  {
                    if (atom[bond[j].a].label != " ")
                      atom[bond[j].b].label = atom[bond[j].a].label;
                    else
                      {
                        bool dashed = false;
                        int n = 0;
                        for (int i = 0; i < n_bond; i++)
                          if (bond[i].exists && i != j && (bond[i].a == bond[j].b || bond[i].b == bond[j].b))
                            {
                              n++;
                              if (bond[i].hash)
                                dashed = true;
                            }
                        if (!dashed)
                          atom[bond[j].b].label = "Xx";
                      }
                  }
              }
            if (not_corner_b)
              {
                bond[j].exists = false;
                atom[bond[j].b].exists = false;
                found = true;
                if (atom[bond[j].a].label == " ")
                  {
                    if (atom[bond[j].b].label != " ")
                      atom[bond[j].a].label = atom[bond[j].b].label;
                    else
                      {
                        bool dashed = false;
                        int n = 0;
                        for (int i = 0; i < n_bond; i++)
                          if (bond[i].exists && i != j && (bond[i].a == bond[j].a || bond[i].b == bond[j].a))
                            {
                              n++;
                              if (bond[i].hash)
                                dashed = true;
                            }
                        if (!dashed)
                          atom[bond[j].a].label = "Xx";
                      }
                  }
              }
          }
    }
}

void mark_terminal_atoms(const vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, int n_atom)
{
  for (int i = 0; i < n_atom; i++)
    atom[i].terminal = false;

  for (int j = 0; j < n_bond; j++)
    if (bond[j].exists && bond[j].type == 1 && !bond[j].arom)
      {
        if (terminal_bond(bond[j].a, j, bond, n_bond))
          atom[bond[j].a].terminal = true;
        if (terminal_bond(bond[j].b, j, bond, n_bond))
          atom[bond[j].b].terminal = true;
      }
}

/**
 * TODO: Returning the vector from the stack causes copy constructor to trigger, which is inefficient.
 * Consider passing the vector as a reference.
 */
vector<vector<int> > find_fragments(const vector<bond_t> &bond, int n_bond, const vector<atom_t> &atom)
{
  vector<vector<int> > frags;
  vector<int> pool;
  int n = 0;

  for (int i = 0; i < n_bond; i++)
    if (bond[i].exists && atom[bond[i].a].exists && atom[bond[i].b].exists)
      pool.push_back(i);

  while (!pool.empty())
    {
      frags.resize(n + 1);
      frags[n].push_back(bond[pool.back()].a);
      frags[n].push_back(bond[pool.back()].b);
      pool.pop_back();
      bool found = true;

      while (found)
        {
          found = false;
          unsigned int i = 0;
          while (i < pool.size())
            {
              bool found_a = false;
              bool found_b = false;
              bool newfound = false;
              for (unsigned int k = 0; k < frags[n].size(); k++)
                {
                  if (frags[n][k] == bond[pool[i]].a)
                    found_a = true;
                  else if (frags[n][k] == bond[pool[i]].b)
                    found_b = true;
                }
              if (found_a && !found_b)
                {
                  frags[n].push_back(bond[pool[i]].b);
                  pool.erase(pool.begin() + i);
                  found = true;
                  newfound = true;
                }
              if (!found_a && found_b)
                {
                  frags[n].push_back(bond[pool[i]].a);
                  pool.erase(pool.begin() + i);
                  found = true;
                  newfound = true;
                }
              if (found_a && found_b)
                {
                  pool.erase(pool.begin() + i);
                  newfound = true;
                }
              if (!newfound)
                i++;
            }
        }
      n++;
    }
  return (frags);
}

int reconnect_fragments(vector<bond_t> &bond, int n_bond, vector<atom_t> &atom, double avg)
{
  vector<vector<int> > frags = find_fragments(bond, n_bond, atom);

  if (frags.size() <= 3)
    {
      for (unsigned int i = 0; i < frags.size(); i++)
        if (frags[i].size() > 2)
          for (unsigned int j = i + 1; j < frags.size(); j++)
            if (frags[j].size() > 2)
              {
                double l = FLT_MAX;
                int atom1 = 0, atom2 = 0;
                for (unsigned int ii = 0; ii < frags[i].size(); ii++)
                  for (unsigned int jj = 0; jj < frags[j].size(); jj++)
                    {
                      double d = atom_distance(atom, frags[i][ii], frags[j][jj]);
                      if (d < l)
                        {
                          l = d;
                          atom1 = frags[i][ii];
                          atom2 = frags[j][jj];
                        }
                    }
                if (l < 1.1 * avg && l > avg / 3)
                  {
                    bond_t b1;
                    bond.push_back(b1);
                    bond[n_bond].a = atom1;
                    bond[n_bond].exists = true;
                    bond[n_bond].type = 1;
                    bond[n_bond].b = atom2;
                    bond[n_bond].curve = atom[atom1].curve;
                    bond[n_bond].hash = false;
                    bond[n_bond].wedge = false;
                    bond[n_bond].up = false;
                    bond[n_bond].down = false;
                    bond[n_bond].Small = false;
                    bond[n_bond].arom = false;
                    bond[n_bond].conjoined = false;
                    n_bond++;
                  }
                if (l < avg / 3)
                  {
                    atom[atom2].x = atom[atom1].x;
                    atom[atom2].y = atom[atom1].y;
                  }
              }
    }

  return (n_bond);
}


/**
 * TODO: Returning the vector from the stack causes copy constructor to trigger, which is inefficient.
 * Consider passing the vector as a reference.
 */
vector<fragment_t> populate_fragments(const vector<vector<int> > &frags, const vector<atom_t> &atom)
{
  vector<fragment_t> r;

  for (unsigned int i = 0; i < frags.size(); i++)
    {
      fragment_t f;
      f.x1 = INT_MAX;
      f.x2 = 0;
      f.y1 = INT_MAX;
      f.y2 = 0;

      for (unsigned j = 0; j < frags[i].size(); j++)
        {
          f.atom.push_back(frags[i][j]);
          if ((int) atom[frags[i][j]].x < f.x1)
            f.x1 = (int) atom[frags[i][j]].x;
          if ((int) atom[frags[i][j]].x > f.x2)
            f.x2 = (int) atom[frags[i][j]].x;
          if ((int) atom[frags[i][j]].y < f.y1)
            f.y1 = (int) atom[frags[i][j]].y;
          if ((int) atom[frags[i][j]].y > f.y2)
            f.y2 = (int) atom[frags[i][j]].y;
        }
      r.push_back(f);
    }
  return (r);
}

// Igor Filippov - 2009.
// The following two functions are adapted from ConfigFile
///
// Class for reading named values from configuration files
// Richard J. Wagner  v2.1  24 May 2004  wagnerr@umich.edu

// Copyright (c) 2004 Richard J. Wagner
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

void trim(string &s)
{
  // Remove leading and trailing whitespace
  static const char whitespace[] = " \n\t\v\r\f";
  s.erase(0, s.find_first_not_of(whitespace));
  s.erase(s.find_last_not_of(whitespace) + 1U);
}

bool load_config_map(const string &file, map<string, string> &out)
{
  typedef string::size_type pos;
  const string& delim = " "; // separator
  const pos skip = delim.length(); // length of separator

  std::ifstream is(file.c_str());
  if (!is)
    return false;

  while (is)
    {
      // Read an entire line at a time
      string line;
      std::getline(is, line);

      // Ignore comments
      //line = line.substr(0, line.find(comm));
      if (line.length() == 0 || line.at(0) == '#')
        continue;

      // replace tabs with spaces
      pos t;
      while ((t = line.find('\t')) != string::npos)
        line[t] = ' ';

      // Parse the line if it contains a delimiter
      pos delimPos = line.find(delim);
      if (delimPos < string::npos)
        {
          // Extract the key
          string key = line.substr(0, delimPos);
          line.replace(0, delimPos + skip, "");

          // Store key and value
          trim(key);
          trim(line);
          out[key] = line; // overwrites if key is repeated
        }
    }

  is.close();

  return true;
}

bool comp_fragments(const fragment_t &aa, const fragment_t &bb)
{
  if (aa.y2 < bb.y1)
    return (true);
  if (aa.y1 > bb.y2)
    return (false);
  if (aa.x1 > bb.x1)
    return (false);
  if (aa.x1 < bb.x1)
    return (true);

  return (false);
}

void find_limits_on_avg_bond(double &min_bond, double &max_bond, const vector<vector<double> > &pages_of_avg_bonds,
                             const vector<vector<double> > &pages_of_ind_conf)
{
  double max_ind_conf = -FLT_MAX;

  for (unsigned int l = 0; l < pages_of_ind_conf.size(); l++)
    for (unsigned int i = 0; i < pages_of_ind_conf[l].size(); i++)
      if (max_ind_conf < pages_of_ind_conf[l][i])
        {
          max_ind_conf = pages_of_ind_conf[l][i];
          min_bond = pages_of_avg_bonds[l][i];
          max_bond = pages_of_avg_bonds[l][i];
        }
  bool flag = true;
  while (flag)
    {
      flag = false;
      for (unsigned int l = 0; l < pages_of_avg_bonds.size(); l++)
        for (unsigned int i = 0; i < pages_of_avg_bonds[l].size(); i++)
          {
            if (pages_of_avg_bonds[l][i] > max_bond && (pages_of_avg_bonds[l][i] - max_bond < 5
                || pages_of_ind_conf[l][i] > max_ind_conf - 0.1))
              {
                max_bond = pages_of_avg_bonds[l][i];
                flag = true;
              }
            if (pages_of_avg_bonds[l][i] < min_bond && (min_bond - pages_of_avg_bonds[l][i] < 5
                || pages_of_ind_conf[l][i] > max_ind_conf - 0.1))
              {
                min_bond = pages_of_avg_bonds[l][i];
                flag = true;
              }
          }
    }
  min_bond--;
  max_bond++;
}


extern job_t *OCR_JOB;
extern job_t *JOB;

// Function: osra_init()
//
// Initialises OSRA library. Should be called at e.g. program startup. This function is automatically called for both SO library and CLI utility.
// See this section for details about library init/cleanup: http://www.faqs.org/docs/Linux-HOWTO/Program-Library-HOWTO.html#INIT-AND-CLEANUP
// Below attribute marker is GNU compiler specific.
void __attribute__ ((constructor)) osra_init()
{
  // Necessary for GraphicsMagick-1.3.8 according to http://www.graphicsmagick.org/1.3/NEWS.html#january-21-2010:
  MagickLib::InitializeMagick(NULL);

  osra_ocr_init();

  srand(1);
}

// Function: osra_destroy()
//
// Releases all resources allocated by OSRA library. Should be called at e.g. program exit. This function is automatically called for both SO library and CLI utility.
// See this section for details about library init/cleanup: http://www.faqs.org/docs/Linux-HOWTO/Program-Library-HOWTO.html#INIT-AND-CLEANUP
// Below attribute marker is GNU compiler specific.
void __attribute__ ((destructor)) osra_destroy()
{
  MagickLib::DestroyMagick();

  osra_ocr_destroy();
}

int osra_process_image(
#ifdef OSRA_LIB
  const char *image_data,
  int image_length,
  ostream &structure_output_stream,
#else
  const string &input_file,
  const string &output_file,
#endif
  int rotate,
  bool invert,
  int input_resolution,
  double threshold,
  int do_unpaper,
  bool jaggy,
  bool adaptive_option,
  const string &output_format,
  const string &embedded_format,
  bool show_confidence,
  bool show_resolution_guess,
  bool show_page,
  bool show_coordinates,
  bool show_avg_bond_length,
  const string &osra_dir,
  const string &spelling_file,
  const string &superatom_file,
  bool debug,
  bool verbose,
  const string &output_image_file_prefix,
  const string &resize
)
{
  // Loading the program data files into maps:
  map<string, string> spelling;

  if (!((spelling_file.length() != 0 && load_config_map(spelling_file, spelling))
        || load_config_map(string(DATA_DIR) + "/" + SPELLING_TXT, spelling) || load_config_map(osra_dir + "/" + SPELLING_TXT, spelling)))
    {
      cerr << "Cannot open " << SPELLING_TXT << " file (tried locations \"" << DATA_DIR << "\", \"" << osra_dir
           << "\"). Specify the custom file location via -l option." << endl;
      return ERROR_SPELLING_FILE_IS_MISSING;
    }

  map<string, string> superatom;

  if (!((superatom_file.length() != 0 && load_config_map(superatom_file, superatom))
        || load_config_map(string(DATA_DIR) + "/" + SUPERATOM_TXT, superatom) || load_config_map(osra_dir + "/"
            + SUPERATOM_TXT, superatom)))
    {
      cerr << "Cannot open " << SUPERATOM_TXT << " file (tried locations \"" << DATA_DIR << "\", \"" << osra_dir
           << "\"). Specify the custom file location via -a option." << endl;
      return ERROR_SUPERATOM_FILE_IS_MISSING;
    }

  if (verbose)
    cout << "spelling (size: " << spelling.size() << ") and superatom (size: " << superatom.size() << ") dictionaries are loaded." << endl;

  string type;

#ifdef OSRA_LIB
  Blob blob(image_data, image_length);
#endif

  try
    {
      Image image_typer;
#ifdef OSRA_LIB
      image_typer.ping(blob);
#else
      image_typer.ping(input_file);
#endif
      type = image_typer.magick();
    }
  catch (...)
    {
      // Unfortunately, GraphicsMagick does not throw exceptions in all cases, so it behaves inconsistent, see
      // https://sourceforge.net/tracker/?func=detail&aid=3022955&group_id=40728&atid=428740
    }

  if (type.empty())
    {
#ifdef OSRA_LIB
      cerr << "Cannot detect blob image type" << endl;
#else
      cerr << "Cannot open file \"" << input_file << '"' << endl;
#endif
      return ERROR_UNKNOWN_IMAGE_TYPE;
    }

  if (verbose)
    cout << "Image type: " << type << '.' << endl;

#ifndef OSRA_LIB
  ofstream outfile;

  if (!output_file.empty())
    {
      outfile.open(output_file.c_str(), ios::out | ios::trunc);
      if (outfile.bad() || !outfile.is_open())
        {
          cerr << "Cannot open file \"" << output_file << "\" for output" << endl;
          return ERROR_OUTPUT_FILE_OPEN_FAILED;
        }
    }
#endif

  if (input_resolution == 0 && (type == "PDF" || type == "PS"))
    input_resolution = 150;

  if (show_coordinates && rotate != 0)
    {
      cerr << "Showing the box coordinates is currently not supported together with image rotation and is therefore disabled." << endl;
#ifdef OSRA_LIB
      return ERROR_ILLEGAL_ARGUMENT_COMBINATION;
#else
      show_coordinates = false;
#endif
    }

  if (!embedded_format.empty() && output_format != "sdf" && (embedded_format != "inchi" || embedded_format == "smi"
      || embedded_format != "can"))
    {
      cerr << "Embedded format option is only possible if output format is SDF and option can have only inchi, smi, or can values." << endl;
      return ERROR_ILLEGAL_ARGUMENT_COMBINATION;
    }

#ifdef OSRA_LIB
  int page = 1;
#else
  int page = count_pages(input_file);
#endif

  vector<vector<string> > pages_of_structures(page, vector<string> (0));
  vector<vector<Image> > pages_of_images(page, vector<Image> (0));
  vector<vector<double> > pages_of_avg_bonds(page, vector<double> (0));
  vector<vector<double> > pages_of_ind_conf(page, vector<double> (0));

  int total_structure_count = 0;

  #pragma omp parallel for default(shared) private(OCR_JOB,JOB)
  for (int l = 0; l < page; l++)
    {
      Image image;
      double page_scale=1;

      if (verbose)
        cout << "Processing page " << (l+1) << " out of " << page << "..." << endl;

      ostringstream density;
      density << input_resolution << "x" << input_resolution;
      image.density(density.str());

      if (type == "PDF" || type == "PS")
        page_scale *= (double) 72 / input_resolution;

#ifdef OSRA_LIB
      image.read(blob);
#else
      ostringstream pname;
      pname << input_file << "[" << l << "]";
      image.read(pname.str());
#endif
      image.modifyImage();
      bool adaptive = convert_to_gray(image, invert, adaptive_option, verbose);

      int num_resolutions = NUM_RESOLUTIONS;
      if (input_resolution != 0)
        num_resolutions = 1;
      vector<int> select_resolution(num_resolutions, input_resolution);
      vector<vector<string> > array_of_structures(num_resolutions);
      vector<vector<double> > array_of_avg_bonds(num_resolutions), array_of_ind_conf(num_resolutions);
      vector<double> array_of_confidence(num_resolutions, -FLT_MAX);
      vector<vector<Image> > array_of_images(num_resolutions);

      if (input_resolution == 0)
        {
          select_resolution[0] = 72;
          select_resolution[1] = 150;
          select_resolution[2] = 300;
          select_resolution[3] = 500;
        }

      if (input_resolution > 300)
        {
          int percent = (100 * 300) / input_resolution;
          ostringstream scale;
          scale << percent << "%";
          image.scale(scale.str());
          page_scale /= (double) percent / 100;
        }

      if (verbose)
        {
          cout << "Input resolutions are ";
          for (vector<int>::iterator it = select_resolution.begin();;)
            {
              cout << *it;

              if (++it < select_resolution.end())
                cout << ", ";
              else
                break;
            }
          cout << '.' << endl;
        }

      ColorGray bgColor = getBgColor(image);
      if (rotate != 0)
        {
          image.backgroundColor(bgColor);
          image.rotate(rotate);
        }

      for (int i = 0; i < do_unpaper; i++)
        unpaper(image);

      // 0.1 is used for THRESHOLD_BOND here to allow for farther processing.
      list<list<list<point_t> > > clusters = find_segments(image, 0.1, bgColor, adaptive, verbose);

      if (verbose)
        cout << "Number of clusters: " << clusters.size() << '.' << endl;

      vector<box_t> boxes;
      int n_boxes = prune_clusters(clusters, boxes);
      std::sort(boxes.begin(), boxes.end(), comp_boxes);

      if (verbose)
        cout << "Number of boxes: " << boxes.size() << '.' << endl;

      // This will hide the output "Warning: non-positive median line gap" from GOCR. Remove after this is fixed:
      fclose(stderr);
      OpenBabel::obErrorLog.StopLogging();

      potrace_param_t * const param = potrace_param_default();
      param->alphamax = 0.;
      //param->turnpolicy = POTRACE_TURNPOLICY_MINORITY;
      param->turdsize = 0;

      for (int res_iter = 0; res_iter < num_resolutions; res_iter++)
        {
          int total_boxes = 0;
          double total_confidence = 0;

          int resolution = select_resolution[res_iter];
          int working_resolution = resolution;
          if (resolution > 300)
            working_resolution = 300;

          double THRESHOLD_BOND;
          THRESHOLD_BOND = threshold;

          if (THRESHOLD_BOND < 0.0001)
            {
              if (resolution >= 150)
                {
                  THRESHOLD_BOND = THRESHOLD_GLOBAL;
                }
              else
                {
                  THRESHOLD_BOND = THRESHOLD_LOW_RES;
                }
            }

          int max_font_height = MAX_FONT_HEIGHT * working_resolution / 150;
          int max_font_width = MAX_FONT_WIDTH * working_resolution / 150;
          bool thick = true;
          if (resolution < 150)
            thick = false;
          else if (resolution == 150 && !jaggy)
            thick = false;

          //Image dbg = image;
          //dbg.modifyImage();
          //dbg.backgroundColor("white");
          //dbg.erase();
          //dbg.type(TrueColorType);
          for (int k = 0; k < n_boxes; k++)
            if ((boxes[k].x2 - boxes[k].x1) > max_font_width && (boxes[k].y2 - boxes[k].y1) > max_font_height
                && !boxes[k].c.empty() && ((boxes[k].x2 - boxes[k].x1) > 2 * max_font_width || (boxes[k].y2
                                           - boxes[k].y1) > 2 * max_font_height))
              {
                int n_atom = 0, n_bond = 0, n_letters = 0, n_label = 0;
                vector<atom_t> atom;
                vector<bond_t> bond;
                vector<atom_t> frag_atom;
                vector<bond_t> frag_bond;
                vector<letters_t> letters;
                vector<label_t> label;
                double box_scale = 1;
                Image orig_box(Geometry(boxes[k].x2 - boxes[k].x1 + 2 * FRAME, boxes[k].y2 - boxes[k].y1 + 2
                                        * FRAME), bgColor);

                for (unsigned int p = 0; p < boxes[k].c.size(); p++)
                  {
                    int x = boxes[k].c[p].x;
                    int y = boxes[k].c[p].y;
                    ColorGray color = image.pixelColor(x, y);
                    //dbg.pixelColor(x, y, color);
                    orig_box.pixelColor(x - boxes[k].x1 + FRAME, y - boxes[k].y1 + FRAME, color);
                  }


                int width = orig_box.columns();
                int height = orig_box.rows();
                Image thick_box;
                if (resolution >= 300)
                  {
                    int max_hist;
                    double nf45;
                    double nf =
                      noise_factor(orig_box, width, height, bgColor, THRESHOLD_BOND, resolution, max_hist, nf45);

                    //if (max_hist < 5) thick = false;
                    if (res_iter == 3)
                      {
                        if (max_hist > 6)
                          {
                            int new_resolution = max_hist * 300 / 4;
                            int percent = (100 * 300) / new_resolution;
                            //resolution = max_hist * select_resolution[res_iter] / 4;
                            resolution = new_resolution;
                            ostringstream scale;
                            scale << percent << "%";
                            orig_box.scale(scale.str());
                            box_scale /= (double) percent/100;
                            working_resolution = 300;
                            thick_box = orig_box;
                            width = thick_box.columns();
                            height = thick_box.rows();
                            nf = noise_factor(orig_box, width, height, bgColor, THRESHOLD_BOND, resolution,
                                              max_hist, nf45);
                          }
                        else
                          {
                            resolution = 500;
                            int percent = (100 * 300) / resolution;
                            ostringstream scale;
                            scale << percent << "%";
                            orig_box.scale(scale.str());
                            box_scale /= (double) percent/100;
                            working_resolution = 300;
                            thick_box = orig_box;
                            width = thick_box.columns();
                            height = thick_box.rows();
                            thick = false;
                            nf = noise_factor(orig_box, width, height, bgColor, THRESHOLD_BOND, resolution,
                                              max_hist, nf45);
                          }
                      }
                    if (jaggy)
                      {
                        orig_box.scale("50%");
                        box_scale *= 2;
                        thick_box = orig_box;
                        working_resolution = 150;
                        width = thick_box.columns();
                        height = thick_box.rows();
                      }
                    else if (nf > 0.5 && nf < 1. && max_hist <= 6)// && res_iter != 3 && max_hist <= 6)
                      try
                        {
                          thick_box = anisotropic_smoothing(orig_box, width, height, 20, 0.3, 1.0, 0.6, 2);
                        }
                      catch (...)
                        {
                          thick_box = orig_box;
                        }
                    /*else if (nf45 > 0.9 && nf45 < 1.2 && max_hist == 3)
                      {
                        //orig_box = anisotropic_smoothing(thick_box, width, height, 60, 0.3, 0.6, 4., 2.);
                        orig_box.scale("50%");
                        thick_box = orig_box;
                        //working_resolution = 150;
                        width = thick_box.columns();
                        height = thick_box.rows();
                        //thick = false;
                    						}*/
                    else
                      thick_box = orig_box;

                  }
                else if (resolution < 300 && resolution > 150)
                  {
                    int nw = width * 300 / resolution;
                    int nh = height * 300 / resolution;
                    thick_box = anisotropic_scaling(orig_box, width, height, nw, nh);
                    width = thick_box.columns();
                    height = thick_box.rows();
                    int percent = (100 * 300) / resolution;
                    ostringstream scale;
                    scale << percent << "%";
                    orig_box.scale(scale.str());
                    box_scale /= (double) percent/100;
                    working_resolution = 300;
                  }
                else
                  thick_box = orig_box;

                if (verbose)
                  cout << "Analysing box " << boxes[k].x1 << "x" << boxes[k].y1 << "-" << boxes[k].x2 << "x" << boxes[k].y2 << " using working resolution " << working_resolution << '.' << endl;

                param->turnpolicy = POTRACE_TURNPOLICY_MINORITY;
                double c_width = 1. * width * 72 / working_resolution;
                double c_height = 1. * height * 72 / working_resolution;
                if (c_height * c_width < SMALL_PICTURE_AREA)
                  param->turnpolicy = POTRACE_TURNPOLICY_BLACK;

                Image box;
                if (thick)
                  box = thin_image(thick_box, THRESHOLD_BOND, bgColor);
                else
                  box = thick_box;

                potrace_bitmap_t * const bm = bm_new(width, height);
                for (int i = 0; i < width; i++)
                  for (int j = 0; j < height; j++)
                    BM_PUT(bm, i, j, get_pixel(box, bgColor, i, j, THRESHOLD_BOND));

                potrace_state_t * const st = potrace_trace(param, bm);
                potrace_path_t const * const p = st->plist;

                n_atom = find_atoms(p, atom, bond, &n_bond);

                int real_font_width, real_font_height;
                n_letters = find_chars(p, orig_box, letters, atom, bond, n_atom, n_bond, height, width, bgColor,
                                       THRESHOLD_BOND, max_font_width, max_font_height, real_font_width, real_font_height,verbose);

                if (verbose)
                  cout << "Number of atoms: " << n_atom << ", bonds: " << n_bond << ", chars: " << n_letters << " after find_atoms()" << endl;

                double avg_bond_length = percentile75(bond, n_bond, atom);

                double max_area = avg_bond_length * 5;
                if (thick)
                  max_area = avg_bond_length;

                n_letters = find_plus_minus(p, letters, atom, bond, n_atom, n_bond, height, width,
                                            real_font_height, real_font_width, n_letters);

                n_atom = find_small_bonds(p, atom, bond, n_atom, &n_bond, max_area, avg_bond_length / 2, 5);

                if (verbose)
                  cout << "Number of atoms: " << n_atom << ", bonds: " << n_bond << ", chars: " << n_letters << " after find_small_bonds()" << endl;

                find_old_aromatic_bonds(p, bond, n_bond, atom, n_atom, avg_bond_length);

                double dist = 3.;
                if (working_resolution < 150)
                  dist = 2;

                double thickness = skeletize(atom, bond, n_bond, box, THRESHOLD_BOND, bgColor, dist, avg_bond_length);

                remove_disconnected_atoms(atom, bond, n_atom, n_bond);
                collapse_atoms(atom, bond, n_atom, n_bond, 3);
                remove_zero_bonds(bond, n_bond, atom);

                n_letters = find_fused_chars(bond, n_bond, atom, letters, n_letters, real_font_height,
                                             real_font_width, 0, orig_box, bgColor, THRESHOLD_BOND, 3, verbose);

                n_letters = find_fused_chars(bond, n_bond, atom, letters, n_letters, real_font_height,
                                             real_font_width, '*', orig_box, bgColor, THRESHOLD_BOND, 5, verbose);

                flatten_bonds(bond, n_bond, atom, 3);
                remove_zero_bonds(bond, n_bond, atom);
                avg_bond_length = percentile75(bond, n_bond, atom);

                if (verbose)
                  cout << "Average bond length: " << avg_bond_length << endl;

                double max_dist_double_bond = dist_double_bonds(atom, bond, n_bond, avg_bond_length);
                n_bond = double_triple_bonds(atom, bond, n_bond, avg_bond_length, n_atom, max_dist_double_bond);

                /*if (ttt++ == 1) {
                	debug_img(orig_box, atom, n_atom, bond, n_bond, "tmp.png");
                }
                */
                n_atom = find_dashed_bonds(p, atom, bond, n_atom, &n_bond, max(MAX_DASH, int(avg_bond_length / 3)),
                                           avg_bond_length, orig_box, bgColor, THRESHOLD_BOND, thick, avg_bond_length);

                n_letters = remove_small_bonds(bond, n_bond, atom, letters, n_letters, real_font_height,
                                               MIN_FONT_HEIGHT, avg_bond_length);

                dist = 4.;
                if (working_resolution < 300)
                  dist = 3;
                if (working_resolution < 150)
                  dist = 2;
                n_bond = fix_one_sided_bonds(bond, n_bond, atom, dist, avg_bond_length);

                n_letters = clean_unrecognized_characters(bond, n_bond, atom, real_font_height, real_font_width, 4,
                            letters, n_letters);

                thickness = find_wedge_bonds(thick_box, atom, n_atom, bond, n_bond, bgColor, THRESHOLD_BOND,
                                             max_dist_double_bond, avg_bond_length, 3, 1);

                n_label = assemble_labels(letters, n_letters, label);

                remove_disconnected_atoms(atom, bond, n_atom, n_bond);

                collapse_atoms(atom, bond, n_atom, n_bond, thickness);

                remove_zero_bonds(bond, n_bond, atom);

                flatten_bonds(bond, n_bond, atom, 2 * thickness);

                remove_zero_bonds(bond, n_bond, atom);

                avg_bond_length = percentile75(bond, n_bond, atom);

                collapse_double_bonds(bond, n_bond, atom, max_dist_double_bond);

                extend_terminal_bond_to_label(atom, letters, n_letters, bond, n_bond, label, n_label, avg_bond_length / 2,
                                              thickness, max_dist_double_bond);

                remove_disconnected_atoms(atom, bond, n_atom, n_bond);
                collapse_atoms(atom, bond, n_atom, n_bond, thickness);
                collapse_doubleup_bonds(bond, n_bond);

                remove_zero_bonds(bond, n_bond, atom);
                flatten_bonds(bond, n_bond, atom, thickness);
                remove_zero_bonds(bond, n_bond, atom);
                remove_disconnected_atoms(atom, bond, n_atom, n_bond);

                extend_terminal_bond_to_bonds(atom, bond, n_bond, avg_bond_length, 2 * thickness, max_dist_double_bond);

                collapse_atoms(atom, bond, n_atom, n_bond, 3);
                remove_zero_bonds(bond, n_bond, atom);
                flatten_bonds(bond, n_bond, atom, 3);
                remove_zero_bonds(bond, n_bond, atom);
                n_letters = clean_unrecognized_characters(bond, n_bond, atom, real_font_height, real_font_width, 0,
                            letters, n_letters);

                assign_charge(atom, bond, n_atom, n_bond, spelling, superatom, debug);
                find_up_down_bonds(bond, n_bond, atom, thickness);
                int real_atoms = count_atoms(atom, n_atom);
		int bond_max_type = 0;
                int real_bonds = count_bonds(bond, n_bond,bond_max_type);


                if (verbose)
                  cout << "Final number of atoms: " << real_atoms << ", bonds: " << real_bonds << ", chars: " << n_letters << '.' << endl;

                if (real_atoms > MIN_A_COUNT && real_atoms < MAX_A_COUNT && real_bonds < MAX_A_COUNT && bond_max_type>0 && bond_max_type<5)
                  {
                    int num_frag;

                    num_frag = resolve_bridge_bonds(atom, n_atom, bond, n_bond, 2 * thickness, avg_bond_length, superatom);
                    collapse_bonds(atom, bond, n_bond, avg_bond_length / 4);
                    collapse_atoms(atom, bond, n_atom, n_bond, 3);
                    remove_zero_bonds(bond, n_bond, atom);
                    extend_terminal_bond_to_bonds(atom, bond, n_bond, avg_bond_length, 7, 0);

                    remove_small_terminal_bonds(bond, n_bond, atom, avg_bond_length);
                    n_bond = reconnect_fragments(bond, n_bond, atom, avg_bond_length);
                    collapse_atoms(atom, bond, n_atom, n_bond, 1);
                    mark_terminal_atoms(bond, n_bond, atom, n_atom);
                    const vector<vector<int> > &frags = find_fragments(bond, n_bond, atom);
                    vector<fragment_t> fragments = populate_fragments(frags, atom);
                    std::sort(fragments.begin(), fragments.end(), comp_fragments);
                    for (unsigned int i = 0; i < fragments.size(); i++)
                      {
                        if (verbose)
                          cout << "Considering fragment #" << i << " " << fragments[i].x1 << "x" << fragments[i].y1 << "-" << fragments[i].x2 << "x"
                               << fragments[i].y2 << ", atoms: " << fragments[i].atom.size() << '.' << endl;

                        if (fragments[i].atom.size() > MIN_A_COUNT)
                          {
                            frag_atom.clear();
                            for (int a = 0; a < n_atom; a++)
                              {
                                frag_atom.push_back(atom[a]);
                                frag_atom[a].exists = false;
                              }

                            for (unsigned int j = 0; j < fragments[i].atom.size(); j++)
                              frag_atom[fragments[i].atom[j]].exists = atom[fragments[i].atom[j]].exists;

                            frag_bond.clear();
                            for (int b = 0; b < n_bond; b++)
                              {
                                frag_bond.push_back(bond[b]);
                              }

                            remove_zero_bonds(frag_bond, n_bond, frag_atom);

                            double confidence = 0;
                            molecule_statistics_t molecule_statistics;
                            int page_number = l + 1;
                            box_t coordinate_box;
                            coordinate_box.x1 = (int) ((double) page_scale * boxes[k].x1 + (double) page_scale * box_scale * fragments[i].x1);
                            coordinate_box.y1 = (int) ((double) page_scale * boxes[k].y1 + (double) page_scale * box_scale * fragments[i].y1);
                            coordinate_box.x2 = (int) ((double) page_scale * boxes[k].x1 + (double) page_scale * box_scale * fragments[i].x2);
                            coordinate_box.y2 = (int) ((double) page_scale * boxes[k].y1 + (double) page_scale * box_scale * fragments[i].y2);

                            string structure =
                              get_formatted_structure(frag_atom, frag_bond, n_bond, output_format, embedded_format,
                                                      molecule_statistics, confidence,
                                                      show_confidence, avg_bond_length, page_scale * box_scale * avg_bond_length,
                                                      show_avg_bond_length,
                                                      show_resolution_guess ? &resolution : NULL,
                                                      show_page ? &page_number : NULL,
                                                      show_coordinates ? &coordinate_box : NULL, superatom);

                            if (verbose)
                              cout << "Structure length: " << structure.length() << ", molecule fragments: " << molecule_statistics.fragments << '.' << endl;


                            if (molecule_statistics.fragments > 0 && molecule_statistics.fragments < MAX_FRAGMENTS && molecule_statistics.num_atoms>MIN_A_COUNT && molecule_statistics.num_bonds>0)
                              {
                                array_of_structures[res_iter].push_back(structure);
                                array_of_avg_bonds[res_iter].push_back(page_scale * box_scale * avg_bond_length);
                                array_of_ind_conf[res_iter].push_back(confidence);
                                total_boxes++;
                                total_confidence += confidence;
                                if (!output_image_file_prefix.empty())
                                  {
                                    Image tmp = image;
                                    Geometry geometry =
                                      (fragments.size() > 1) ? Geometry(box_scale * fragments[i].x2 - box_scale * fragments[i].x1 + 4 * real_font_width, //
                                                                        box_scale * fragments[i].y2 - box_scale * fragments[i].y1 + 4 * real_font_height, //
                                                                        boxes[k].x1 + box_scale * fragments[i].x1 - FRAME - 2 * real_font_width, //
                                                                        boxes[k].y1 + box_scale * fragments[i].y1 - FRAME - 2 * real_font_height)
                                      : Geometry(boxes[k].x2 - boxes[k].x1, boxes[k].y2 - boxes[k].y1, boxes[k].x1, boxes[k].y1);

                                    try
                                      {
                                        tmp.crop(geometry);
                                      }
                                    catch (...)
                                      {
                                        tmp = orig_box;
                                      }

                                    array_of_images[res_iter].push_back(tmp);
                                  }
                              }
                          }
                      }
                  }

                if (st != NULL)
                  potrace_state_free(st);
                if (bm != NULL)
                  {
                    free(bm->map);
                    free(bm);
                  }
              }
          if (total_boxes > 0)
            array_of_confidence[res_iter] = total_confidence / total_boxes;
          //dbg.write("debug.png");
        }
      potrace_param_free(param);

      double max_conf = -FLT_MAX;
      int max_res = 0;
      for (int i = 0; i < num_resolutions; i++)
        {
          if (array_of_confidence[i] > max_conf && array_of_structures[i].size() > 0)
            {
              max_conf = array_of_confidence[i];
              max_res = i;
            }
        }
      #pragma omp critical
      {
        for (unsigned int i = 0; i < array_of_structures[max_res].size(); i++)
          {
            pages_of_structures[l].push_back(array_of_structures[max_res][i]);
            if (!output_image_file_prefix.empty())
              pages_of_images[l].push_back(array_of_images[max_res][i]);
            pages_of_avg_bonds[l].push_back(array_of_avg_bonds[max_res][i]);
            pages_of_ind_conf[l].push_back(array_of_ind_conf[max_res][i]);
            total_structure_count++;
          }
      }
    }

  double min_bond = -FLT_MAX, max_bond = FLT_MAX;
  if (total_structure_count >= STRUCTURE_COUNT)
    find_limits_on_avg_bond(min_bond, max_bond, pages_of_avg_bonds, pages_of_ind_conf);
  // If multiple pages are processed at several  resolutions different pages
  // may be processed at different resolutions leading to a seemingly different average bond length
  // Currently multi-page documents (PDF and PS) are all processed at the same resolution
  // and single-page images have all structures on the page at the same resolution

  //cout << min_bond << " " << max_bond << endl;

#ifdef OSRA_LIB
  ostream &out_stream = structure_output_stream;
#else
  ostream &out_stream = outfile.is_open() ? outfile : cout;
#endif

#ifdef OSRA_ANDROID
  // For Andriod version we will find the structure with maximum confidence value, as the common usecase for Andriod is to analyse the
  // image (taken by embedded photo camera) that usually contains just one molecule:
  double max_confidence = -FLT_MAX;
  int l_index = 0;
  int i_index = 0;
#endif

  int image_count = 0;

  for (int l = 0; l < page; l++)
    for (unsigned int i = 0; i < pages_of_structures[l].size(); i++)
      if (pages_of_avg_bonds[l][i] > min_bond && pages_of_avg_bonds[l][i] < max_bond)
        {
#ifdef OSRA_ANDROID
          if (pages_of_ind_conf[l][i] > max_confidence)
            {
              max_confidence = pages_of_ind_conf[l][i];
              l_index = l;
              i_index = i;
            }
#else
          out_stream << pages_of_structures[l][i];
#endif
          // Dump this structure into a separate file:
          if (!output_image_file_prefix.empty())
            {
              ostringstream fname;
              fname << output_image_file_prefix << image_count << ".png";
              image_count++;
              if (fname.str() != "")
                {
                  Image tmp = pages_of_images[l][i];
                  if (resize != "")
                    {
                      tmp.scale(resize);
                    }
                  tmp.write(fname.str());
                }
            }
        }

#ifdef OSRA_ANDROID
  // Output the structure with maximum confidence value:
  out_stream << pages_of_structures[l_index][i_index];
#endif

  out_stream.flush();

#ifndef OSRA_LIB
  if (!output_file.empty())
    outfile.close();
#endif

  return 0;
}
