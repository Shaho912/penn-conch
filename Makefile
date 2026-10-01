######################################################################
#
#                       Author: Hannah Pan
#                       Date:   01/13/2021
#
# The autograder will run the following command to build the program:
#
#       make -B
#
# To build a version that does not call kill(2), it will run:
#
#       make -B CPPFLAGS=-DEC_NOKILL
#
######################################################################

# name of the program to build
#
PROG=penn-conch

PROMPT='"$(PROG)> "'

# Remove -DNDEBUG during development if assert(3) is used
#
override CPPFLAGS += -DNDEBUG -DPROMPT=$(PROMPT)

CC = clang-19

# Replace -O1 with -g for a debug version during development
#
CFLAGS += -g3 -gdwarf-4 -Wall -Werror -Wno-gnu -I. -I.. --std=gnu2x
CXXFLAGS += -g3 -gdwarf-4 -Wall -Werror -Wno-gnu -I. -I.. --std=gnu++2b

SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
HEADERS = $(wildcard *.h)

YOUR_SRCS = $(filter-out parser.c, $(SRCS))
YOUR_HEADERS = $(filter-out parser.h, $(HEADERS))

.PHONY : all clean tidy-check format

all: $(PROG) tidy-check

$(PROG) : $(OBJS) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

clean :
	$(RM) $(OBJS) $(PROG)

tidy-check: 
	clang-tidy-19 \
        --extra-arg=--std=gnu2x \
        -warnings-as-errors=* \
        -header-filter=.* \
        $(YOUR_SRCS) $(YOUR_HEADERS)

format:
	clang-format-19 -i --verbose --style=Chromium $(YOUR_SRCS) $(YOUR_HEADERS)

