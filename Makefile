# Project folder names
SRCDIR = src
LIBDIR = lib
INCDIR = include
OBJDIR = obj
BINDIR = bin
TESTDIR = test
TMPDIR = tmp
DIST_NAME = $(shell basename $$PWD)

SHELL = /bin/bash
CC = gcc
AR = ar
STD = -std=c99
DEBUG = -g
OPTFLAGS = -O3
WARNS = -Wextra -Wall -Werror -pedantic
CFLAGS = $(STD) $(DEBUG) $(OPTFLAGS) $(WARNS)
ARFLAGS = rvs
INCLUDES = -I $(INCDIR)

TARGETS = $(BINDIR)/server $(BINDIR)/client

.PHONY: all clean cleanall test1 test2 sample_files dist
# Delete default suffixes
.SUFFIXES:
.SUFFIXES: .c .h
.SILENT: test1 test2 dist

all : $(TARGETS)

$(BINDIR)/server: $(OBJDIR)/free_item.o $(OBJDIR)/readnwrite.o $(OBJDIR)/str2num.o $(OBJDIR)/config_parser.o $(OBJDIR)/storage.o $(OBJDIR)/ubuffer.o $(OBJDIR)/icl_hash.o $(OBJDIR)/server.o | $(BINDIR) $(TMPDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -pthread

$(BINDIR)/client: $(OBJDIR)/free_item.o $(OBJDIR)/str2num.o $(OBJDIR)/client.o $(LIBDIR)/libfssapi.a | $(BINDIR) $(TMPDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

$(LIBDIR)/libfssapi.a: $(OBJDIR)/free_item.o $(OBJDIR)/readnwrite.o $(OBJDIR)/str2num.o $(OBJDIR)/fss_api.o | $(LIBDIR)
	$(AR) $(ARFLAGS) $@ $^

# Dependencies
$(OBJDIR)/config_parser.o: $(SRCDIR)/config_parser.c $(INCDIR)/config_parser.h $(INCDIR)/fss_defaults.h $(INCDIR)/error_handling.h $(INCDIR)/free_item.h $(INCDIR)/str2num.h
$(OBJDIR)/storage.o: $(SRCDIR)/storage.c $(INCDIR)/storage.h $(INCDIR)/icl_hash.h $(INCDIR)/communication_protocol.h $(INCDIR)/error_handling.h $(INCDIR)/concurrency.h $(INCDIR)/free_item.h
$(OBJDIR)/ubuffer.o: $(SRCDIR)/ubuffer.c $(INCDIR)/ubuffer.h $(INCDIR)/concurrency.h
$(OBJDIR)/icl_hash.o: $(SRCDIR)/icl_hash.c $(INCDIR)/icl_hash.h
$(OBJDIR)/fss_api.o: $(SRCDIR)/fss_api.c $(INCDIR)/fss_api.h $(INCDIR)/posixver.h $(INCDIR)/communication_protocol.h $(INCDIR)/error_handling.h $(INCDIR)/free_item.h $(INCDIR)/readnwrite.h $(INCDIR)/str2num.h
$(OBJDIR)/str2num.o: $(SRCDIR)/str2num.c $(INCDIR)/str2num.h
$(OBJDIR)/readnwrite.o: $(SRCDIR)/readnwrite.c $(INCDIR)/readnwrite.h
$(OBJDIR)/free_item.o: $(SRCDIR)/free_item.c $(INCDIR)/free_item.h

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Create the directories, if they do not already exist
$(LIBDIR) $(OBJDIR) $(BINDIR) $(TMPDIR):
	mkdir -p $@

# Delete all files that are normally created by building the program
clean:
	rm -f $(TARGETS)

# Delete all files created by this makefile (leave only the files that were in the distribution)
cleanall: clean
	rm -rf $(OBJDIR) $(BINDIR) $(LIBDIR) $(TMPDIR)

# Test targets
test1: all
	./$(TESTDIR)/test1.sh

test2: all
	./$(TESTDIR)/test2.sh

# To be implemented...
sample_files:
	;

# Create a distribution tar file for this program
dist: cleanall
	cd ..; \
	tar -cf $(DIST_NAME).tar $(DIST_NAME); \
	gzip -f9 $(DIST_NAME).tar;
