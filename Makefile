# Arcade Station - emulador arcade (nucleo FB Alpha 2012, solo arcade) para PS3 (PSL1GHT), basado en Katari v0.1.4 / NesNet v1.9.9
# Requiere: PS3DEV y PSL1GHT en el entorno, y el nucleo FBA 2012 ya compilado en
# fba/svn-current/trunk/fbalpha2012_libretro_psl1ght.a (lo hace el workflow de GitHub Actions).

TARGET   = ArcadeStation
TITLE    = Arcade Station
APPID    = ARCA00001
CONTENTID = UP0001-$(APPID)_00-0000000000000000

FBA      = fba/svn-current/trunk

# El nucleo FBA 2012 se compila como libreria estatica; aqui solo va main.c.
# El nucleo es C++: se enlaza con libstdc++.
OFILES   = source/main.o

CFLAGS   = -O2 -Wall -std=gnu99 -mcpu=cell -mminimal-toc $(MACHDEP) -DNESNET_PS3 \
           -I$(PS3DEV)/ppu/include -I$(PSL1GHT)/ppu/include -I$(PSL1GHT)/ppu/include/simdmath \
           -I$(FBA)/src/burner/libretro -I$(FBA)/src/burner/libretro/libretro-common/include

LDFLAGS  = $(MACHDEP)

LIBS     = $(FBA)/fbalpha2012_libretro_psl1ght.a \
           -lrsx -lgcm_sys -lio -laudio -lsysfs -lsysutil -lnetctl -lnet -lsysmodule -lrt -llv2 \
           -lstdc++ -lsupc++ -lm

# Todo lo que haya en data/ se copia dentro del .pkg (data/USRDIR/config.txt)
PKGFILES = data

BUILDDIR = build

# Icono del XMB (320x176) en la raiz del repo; si no esta, PSL1GHT usa uno generico
ifneq ($(wildcard $(CURDIR)/ICON0.PNG),)
ICON0    = $(CURDIR)/ICON0.PNG
endif

include $(PSL1GHT)/ppu_rules

# make trae LD=ld por defecto y base_rules solo lo cambia si no existe: forzar ppu-gcc
LD = $(CC)

pkg: $(TARGET).pkg

clean:
	rm -rf $(TARGET)*.pkg $(TARGET).self $(TARGET).elf $(OFILES) $(BUILDDIR)
