RA_INCLUDE = -I../../

JSON_DIR   = ../../3rdparty/json

VOICE_OBJ = $(patsubst %.cpp,obj/%.o,$(wildcard *.cpp))
VOICE_H   = $(wildcard *.hpp)

VOICE_INCLUDE = \
	$(RA_INCLUDE) \
	-I$(JSON_DIR)/include

VOICE_DEFS = -DWITH_SQL

HAVE_MYSQL=yes
ifeq ($(HAVE_MYSQL),yes)
	SERVER_DEPENDS=voice-server
else
	SERVER_DEPENDS=needs_mysql
endif
ALL_DEPENDS=server



#####################################################################
.PHONY: all server clean help needs_mysql

all: $(ALL_DEPENDS)

server: $(SERVER_DEPENDS)

clean:
	@echo "	CLEAN	voice"
	@rm -rf obj ../../voice-server

help:
	@echo "possible targets are 'server' 'all' 'clean' 'help'"
	@echo "'server'  - voice server"
	@echo "'all'     - builds all above targets"
	@echo "'clean'   - cleans builds and objects"
	@echo "'help'    - outputs this message"

#####################################################################

needs_mysql:
	@echo "MySQL not found or disabled by the configure script"
	@exit 1

obj:
	@echo "	MKDIR	$@"
	@mkdir -p $@

voice-server: $(VOICE_OBJ)
	@echo "	LD	voice-server"
	@g++  -L/usr/lib -o ../../voice-server $(VOICE_OBJ) -lpcre -lz  -ldl -L/usr/lib64/ -lmariadb -lpthread

obj/%.o: %.cpp $(VOICE_H) | obj
	@echo "	CXX	$<"
	@g++ -g -O2 -std=c++17 $(VOICE_INCLUDE) $(VOICE_DEFS) -I/usr/include/mysql -I/usr/include/mysql/mysql  -I../common -DHAS_TLS -fno-strict-aliasing -DHAVE_SETRLIMIT -DHAVE_STRNLEN  -I/usr/include -DHAVE_MONOTONIC_CLOCK -c -o $@ $<
