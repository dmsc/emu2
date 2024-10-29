# Detect OS with `uname -s`
UNAME_S:=$(shell uname -s 2> /dev/null)

# Detect if CC is GCC or Clang with `-v`
GCC_CLANG=$(shell $(CC) -v 2>&1 | \
 grep -q -i -e "gcc version" -e "clang version" 2> /dev/null && \
  printf '%s\n' "1")

# Detect if CC is Sun CC with `-V`
ifneq ($(GCC_CLANG),1)
 SUNCC_CMP=$(shell $(CC) -V 2>&1 | \
  grep -q -e "Sun C" 2> /dev/null && printf '%s\n' "1")
endif

# Sun CC: Disable LTO
ifeq ($(SUNCC_CMP),1)
 NO_LTO:=1
endif

# Detect if CC is GCC by name
ifneq "$(findstring gcc,$(CC))" ""
 GCC_CLANG=1
endif

# Detect if CC is Clang by name
ifneq "$(findstring clang,$(CC))" ""
 GCC_CLANG=1
endif

# Detect if GCC (or Clang) needs `-std=gnu99`
ifeq ($(GCC_CLANG),1)
 C99_WR:=$(shell printf '%s\n' \
  "int main(void){for(int i=0;i<1;++i);return 0;}" > .test.c; \
   $(CC) .test.c -o .test.out > /dev/null 2>&1; \
    echo $$?; rm -f .test.c .test.out > /dev/null 2>&1)
 ifeq ($(C99_WR),1)
  C99_OK:=$(shell printf '%s\n' \
   "int main(void){for(int i=0;i<1;++i);return 0;}" > .test.c; \
    $(CC) -std=gnu99 .test.c -o .test.out > /dev/null 2>&1; \
     echo $$?; rm -f .test.c .test.out > /dev/null 2>&1)
 endif
 ifeq ($(C99_OK),0)
  CFLAGS+=-std=gnu99
 endif
endif

# Extra CFLAGS for GCC or Clang
EXTRA_CFLAGS?=-Wall -g -Werror=implicit-function-declaration
ifeq ($(GCC_CLANG),1)
 CFLAGS+=$(EXTRA_CFLAGS)
endif

# Detect if CC supports `-flto`
ifndef NO_LTO
 FLTO_WR:=$(shell printf '%s\n' "int main(void){return 0;}" > .test.c; \
  $(CC) -flto .test.c -o .test.out > /dev/null 2>&1; \
   echo $$?; rm -f .test.c .test.out > /dev/null 2>&1)
 ifeq ($(FLTO_WR),0)
  FLTO_OK:=$(shell printf '%s\n' "int main(void){return 0;}" > .test.c; \
   $(CC) -Werror -flto .test.c -o .test.out > /dev/null 2>&1; \
    echo $$?; rm -f .test.c .test.out > /dev/null 2>&1)
  ifeq ($(FLTO_OK),0)
   LTO_FLAGS:=-flto
   # Detect if CC supports `-flto=auto`
   AUTO_WR:=$(shell printf '%s\n' "int main(void){return 0;}" > .test.c; \
    $(CC) -flto=auto .test.c -o .test.out > /dev/null 2>&1; \
     echo $$?; rm -f .test.c .test.out > /dev/null 2>&1)
   ifeq ($(AUTO_WR),0)
    AUTO_OK:=$(shell printf '%s\n' "int main(void){return 0;}" > .test.c; \
     $(CC) -Werror -flto=auto .test.c -o .test.out > /dev/null 2>&1; \
      echo $$?; rm -f .test.c .test.out > /dev/null 2>&1)
    ifeq ($(AUTO_OK),0)
     LTO_FLAGS:=-flto=auto
    endif
   endif
  endif
 endif
 CFLAGS+=$(LTO_FLAGS)
 LDFLAGS+=$(LTO_FLAGS)
endif

# Solaris or illumos: Force `-flto=auto` to `-flto`
ifneq "$(findstring SunOS,$(UNAME_S))" ""
 CFLAGS := $(subst -flto=auto,-flto,$(CFLAGS))
 LDFLAGS := $(subst -flto=auto,-flto,$(LDFLAGS))
endif

# AIX: Default to 64-bit (OBJECT_MODE=64)
ifneq "$(findstring AIX,$(UNAME_S))" ""
 export OBJECT_MODE=64
 # AIX with GCC: Disable LTO, use 64-bit
 ifneq "$(findstring gcc,$(CC))" ""
  CFLAGS+=-maix64
  LDFLAGS+=-maix64
  CFLAGS:=$(subst -flto=auto,-flto,$(CFLAGS))
  CFLAGS:=$(subst -flto,,$(CFLAGS))
  LDFLAGS:=$(subst -flto=auto,-flto,$(LDFLAGS))
  LDFLAGS:=$(subst -flto,,$(LDFLAGS))
 endif
endif

# OS/400 with GCC: Disable LTO
ifneq "$(findstring OS400,$(UNAME_S))" ""
 ifneq "$(findstring gcc,$(CC))" ""
  CFLAGS:=$(subst -flto=auto,-flto,$(CFLAGS))
  CFLAGS:=$(subst -flto,,$(CFLAGS))
  LDFLAGS:=$(subst -flto=auto,-flto,$(LDFLAGS))
  LDFLAGS:=$(subst -flto,,$(LDFLAGS))
 endif
endif
