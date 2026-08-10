-include config.mk

VERSION = 1.0.0

CFLAGS_STD ?= -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -DNDEBUG
CFLAGS_STD += -DVERSION=\"${VERSION}\"

LDFLAGS_STD ?= -lc -lutil

STRIP ?= strip
INSTALL ?= install

PREFIX ?= /usr/local
SHAREDIR ?= ${PREFIX}/share

SRC = amux.c

all: amux

config.h:
	cp config.def.h config.h

config.mk:
	@touch $@

amux: config.h config.mk *.c
	${CC} ${CFLAGS} ${CFLAGS_STD} ${CFLAGS_AUTO} ${CFLAGS_EXTRA} ${SRC} ${LDFLAGS} ${LDFLAGS_STD} ${LDFLAGS_AUTO} -o $@

debug: clean
	make CFLAGS_EXTRA='${CFLAGS_DEBUG}'

# The Python suite allocates its own pseudo terminals, so it runs headless
# (in CI, over ssh, anywhere). testsuite.sh instead compares byte-exact
# terminal output and therefore needs a real tty on stdin; run it by hand.
check: amux
	@status=0; \
	for t in tests/*.py; do \
		echo "== $$t"; \
		python3 "$$t" || status=1; \
	done; \
	exit $$status

# Re-render the README demo. See demo/README.md.
demo: amux
	@command -v vhs >/dev/null 2>&1 || { \
		echo "vhs is not installed: https://github.com/charmbracelet/vhs"; \
		echo "  brew install vhs   (or a release binary)"; \
		exit 1; \
	}; \
	vhs demo/reattach.tape

clean:
	@echo cleaning
	@rm -f amux amux-*.tar.gz

dist: clean
	@echo creating dist tarball
	@git archive --prefix=amux-${VERSION}/ -o amux-${VERSION}.tar.gz HEAD

installdirs:
	@${INSTALL} -d ${DESTDIR}${PREFIX}/bin \
		${DESTDIR}${MANPREFIX}/man1

install: amux installdirs
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@${INSTALL} -m 0755 amux ${DESTDIR}${PREFIX}/bin
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@sed "s/VERSION/${VERSION}/g" < amux.1 > ${DESTDIR}${MANPREFIX}/man1/amux.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/amux.1

install-strip: install
	${STRIP} ${DESTDIR}${PREFIX}/bin/amux

install-completion:
	@echo installing zsh completion file to ${DESTDIR}${SHAREDIR}/zsh/site-functions
	@install -Dm644 contrib/amux.zsh ${DESTDIR}${SHAREDIR}/zsh/site-functions/_amux

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/amux
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/amux.1
	@echo removing zsh completion file from ${DESTDIR}${SHAREDIR}/zsh/site-functions
	@rm -f ${DESTDIR}${SHAREDIR}/zsh/site-functions/_amux

.PHONY: all check demo clean dist install installdirs install-strip install-completion uninstall debug
