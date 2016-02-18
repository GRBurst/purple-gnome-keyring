TARGET = purple-gnome-keyring

VERSION = "1.0"
LIBSECRET	= `pkg-config --libs --cflags libsecret-1`
DBUSLIB		= `pkg-config --cflags dbus-glib-1`
PURPLE		= `pkg-config --cflags purple`

all: ${TARGET}.so

clean: 
	rm -f ${TARGET}.so

${TARGET}.so: ${TARGET}.c

	${CC} ${CFLAGS} ${LDFLAGS} -Wall -I. -g -O2 ${TARGET}.c -o ${TARGET}.so -shared -fPIC -DPIC -ggdb ${PURPLE} ${LIBSECRET} ${DBUSLIB}

install: ${TARGET}.so
	mkdir -p ~/.purple/plugins
	cp ${TARGET}.so ~/.purple/plugins/

