
ROOT = $(HOME)/emacs
MYSQL_DIR = $(HOME)/mysqlinst
CC      = gcc
LD      = gcc
CFLAGS  = -ggdb3 -Wall
LDFLAGS =

SRCS = mysql-el.c
OBJS = $(SRCS:.c=.o)

all: mysql-el.so mysql-el.info

mysql-el.so: $(OBJS)
	$(LD) -shared $(LDFLAGS) -o $@ $(OBJS) -L$(MYSQL_DIR)/lib -Wl,-rpath,$(MYSQL_DIR)/lib -lmysqlclient

%.o: %.c
	$(CC) $(CFLAGS) -I$(ROOT)/src -I$(MYSQL_DIR)/include -fPIC -c $<

%.info: %.texi
	makeinfo $< -o $@

clean:
	rm -f $(OBJS) *.so *.info

.PHONY: all clean

