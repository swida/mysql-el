
ROOT = $(HOME)/emacs
MYSQL_DIR = $(HOME)/mysqlinst
CC      = gcc
LD      = gcc
CFLAGS  = -ggdb3 -Wall
LDFLAGS =

all: mysql-el.so mysql-el.info

%.so: %.o
	$(LD) -shared $(LDFLAGS) -o $@ $< -L$(MYSQL_DIR)/lib -lmysqlclient

%.o: %.c
	$(CC) $(CFLAGS) -I$(ROOT)/src  -I$(MYSQL_DIR)/include -fPIC -c $<

%.info: %.texi
	makeinfo $< -o $@

