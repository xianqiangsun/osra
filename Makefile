######################### GCC Setup ###################
X11LIBS=-L/usr/X11R6/lib
#X11LIBS=-L/usr/X11R6/lib64
IMLIBS= $(X11LIBS) -lMagick++ -lWand -lMagick -ltiff -lfreetype -ljpeg -lXext -lSM -lICE -lX11 -lXt -lbz2
#IMLIBS=-L/usr/local/lib -lMagick++ -lWand -lMagick -lgdi32 -ljbig -llcms -ltiff -ljasper  -ljpeg  -lpng -lbz2 -lz 
POTRACELIB=-L../../potrace-1.7/src/ -lpotrace
POTRACEINC=-I../../potrace-1.7/src/
GOCRSRC=../../gocr-0.45/src/
GOCRINC= -I$(GOCRSRC) -I../../gocr-0.45/include/
GOCRLIB= -L$(GOCRSRC) -lPgm2asc -lnetpbm
#OPENBABELLIB=../openbabel-2.1.1/src/formats/cansmilesformat.o ../openbabel-2.1.1/src/formats/obmolecformat.o -L/usr/local/lib -lopenbabel
OPENBABELLIB=-L/usr/local/lib -lopenbabel   
OPENBABELINC=-I/usr/local/include/openbabel-2.0/
TCLAPINC=-I/usr/local/include/tclap/ -I/usr/local/include
OCRADOBJ = arg_parser.o common.o rational.o rectangle.o track.o ucs.o \
       page_image.o page_image_io.o page_image_layout.o \
       bitmap.o block.o profile.o feats.o feats_test0.o feats_test1.o \
       character.o character_r11.o character_r12.o character_r13.o \
       textline.o textline_r2.o textblock.o textpage.o main.o

CPP = g++ -g -O2 -fPIC -I/usr/local/include -D_LIB -D_MT -Wall -DHAVE_CONFIG_H $(POTRACEINC) $(GOCRINC) $(OPENBABELINC) $(TCLAPINC)
LD=g++ -g -O2 -fPIC
CP=cp
SED=sed

LIBS=$(POTRACELIB) -lm  $(IMLIBS) $(GOCRLIB) $(OPENBABELLIB) -lz
OBJ = osra.o osra_mol.o  osra_anisotropic.o osra_ocr.o $(OCRADOBJ)


all:	$(OBJ)
	${LD}  -o osra $(OPTS) $(OBJ) $(LIBS)


osra.o:	osra.cpp osra.h pgm2asc.h output.h list.h unicode.h gocr.h
	$(CPP) -c osra.cpp

osra_mol.o: osra_mol.cpp osra.h
	    $(CPP) -c osra_mol.cpp

osra_anisotropic.o:	osra_anisotropic.cpp osra.h
	$(CPP) -c osra_anisotropic.cpp

osra_ocr.o:	osra_ocr.cpp osra.h
	$(CPP) -c osra_ocr.cpp
	
clean:	
	rm -f *.o osra pgm2asc.h output.h list.h unicode.h gocr.h

pgm2asc.h:
	$(CP) $(GOCRSRC)/pgm2asc.h ./
output.h:
	$(CP) $(GOCRSRC)/output.h ./
gocr.h:
	$(CP) $(GOCRSRC)/gocr.h ./	
unicode.h:
	$(SED) '/INFINITY/d' $(GOCRSRC)/unicode.h >unicode.h
list.h:
	$(SED) 's/struct\ list/struct\ list\_s/' $(GOCRSRC)/list.h >list.h

%.o : %.cc
	$(CPP)  -c -o $@ $<

$(OCRADOBJ)             : bitmap.h block.h common.h rational.h rectangle.h ucs.h
arg_parser.o        : arg_parser.h
character.o         : character.h
character_r11.o     : character.h profile.h feats.h
character_r12.o     : character.h profile.h feats.h
character_r13.o     : character.h profile.h feats.h
feats.o             : profile.h feats.h
feats_test0.o       : profile.h feats.h
feats_test1.o       : profile.h feats.h
main.o              : arg_parser.h page_image.h textpage.h
page_image.o        : page_image.h
page_image_io.o     : page_image.h
page_image_layout.o : track.h page_image.h
profile.o           : profile.h
textblock.o         : track.h character.h page_image.h textline.h textblock.h
textline.o          : track.h character.h page_image.h textline.h
textline_r2.o       : track.h character.h textline.h
textpage.o          : track.h character.h page_image.h textline.h textblock.h textpage.h
track.o             : track.h

