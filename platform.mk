# Determine if CC is GCC or Clang
TGCLT:=$(shell $(CC) --version 2>&1 | grep -q -i -e 'clang' -e 'gcc' && \
         printf '%s\n' "1" || printf '%s\n' "0")
ifeq "$(TGCLT)" "1"
 GCC_CL=1
endif

# If CC is GCC or Clang, use extra warning flags
EXTRA_CFLAGS := -Wall -Werror=implicit-function-declaration -Werror=int-conversion
ifeq "$(GCC_CL)" "1"
 CFLAGS += $(EXTRA_CFLAGS)
endif

# Determine OS via `uname -s`
UNAME_S:=$(shell uname -s 2> /dev/null)

# Determine LTO invocation: Try `-flto=auto`, fall back to `-ftlo`
ifndef NO_LTO
 LTOSYN:=$(shell $(CC) -flto=auto -E - < /dev/null > /dev/null 2>&1 && \
          printf '%s\n' "-flto=auto" || printf '%s\n' "-flto")
 ifeq "$(findstring pcc,$(CC))" ""
  ifeq "$(findstring xlc,$(CC))" ""
   ifeq "$(findstring nvc,$(CC))" ""
    ifeq "$(findstring suncc,$(CC))" ""
     ifeq "$(findstring clang,$(CC))" ""
      ifeq "$(findstring gcc,$(CC))" ""
       ifeq "$(findstring icc,$(CC))" ""
        # Others? try LTO
        CFLAGS += $(LTOSYN)
        LDFLAGS += $(LTOSYN)
       else
        # Intel C Compiler Classic: use IPO
        CFLAGS += -ipo -diag-disable=10440
        LDFLAGS += -ipo -diag-disable=10440
       endif
      else
       # GCC
       CFLAGS += $(LTOSYN)
       LDFLAGS += $(LTOSYN)
      endif
     else
      # Clang
      CFLAGS += $(LTOSYN) -Wno-ignored-optimization-argument
      LDFLAGS += $(LTOSYN) -Wno-ignored-optimization-argument
     endif
    else
     # Oracle Studio: no LTO
    endif
   else
    # NVIDIA HPC SDK: no LTO
   endif
  else
   # IBM XLC (V16): no LTO
  endif
 else
 # Portable C Compiler: no LTO
 endif

 # Solaris or illumos: Force `-flto=auto` to `-flto`
 ifneq "$(findstring SunOS,$(UNAME_S))" ""
  CFLAGS := $(subst -flto=auto,-flto,$(CFLAGS))
  LDFLAGS := $(subst -flto=auto,-flto,$(LDFLAGS))
 endif

 # AIX with GCC: no LTO
 ifneq "$(findstring AIX,$(UNAME_S))" ""
  ifneq "$(findstring gcc,$(CC))" ""
   CFLAGS := $(subst -flto=auto,-flto,$(CFLAGS))
   CFLAGS := $(subst -flto,,$(CFLAGS))
   LDFLAGS := $(subst -flto=auto,-flto,$(LDFLAGS))
   LDFLAGS := $(subst -flto,,$(LDFLAGS))
  endif
 endif

 # OS/400 with GCC: no LTO
 ifneq "$(findstring OS400,$(UNAME_S))" ""
  ifneq "$(findstring gcc,$(CC))" ""
   CFLAGS := $(subst -flto=auto,-flto,$(CFLAGS))
   CFLAGS := $(subst -flto,,$(CFLAGS))
   LDFLAGS := $(subst -flto=auto,-flto,$(LDFLAGS))
   LDFLAGS := $(subst -flto,,$(LDFLAGS))
  endif
 endif
endif
