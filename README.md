perfevo
=======

static checkers for performance bugs

How to build llvm-2.8 and binutils-2.20.90?

0. cd ${LLVM_ROOT}

1. mkdir install

2. wget ftp://sourceware.org/pub/binutils/snapshots/binutils-2.20.90.tar.bz2

3. tar -xvjpf binutils-2.20.90.tar.bz2

4. cd binutils-2.20.90/

5. mkdir build

6. cd build

7. ../configure --prefix=${LLVM_ROOT}/install  -enable-gold --enable-binutils --enable-plugins

8. make all-gold all-binutils -j8

9. make install-gold install-binutils

10. cd ${LLVM_ROOT}

11. wget http://llvm.org/releases/2.8/llvm-2.8.tgz

12. tar xvzf llvm-2.8.tgz

13. cd llvm-2.8/

14. mkdir build

15. cd build

16. ../configure --prefix=${LLVM_ROOT}/install/ -with-binutils-include=${LLVM_ROOT}/binutils-2.20.90/include --enable-pic

17. make ENABLE_OPTIMIZED=1 -j8

18. make install


How to build perfevo?

1. edit ./autoconf/configure.ac

1.1. LLVM_SRC_ROOT=${LLVM_ROOT}/llvm-2.8

1.2. LLVM_OBJ_ROOT=${LLVM_ROOT}/llvm-2.8/build

2. cd ./autoconf

3. ./AutoRegen.sh        //provide llvm directory and llvm-building directory when running this command

4. cd ..

5. mkdir build

6. cd build

7. ../configure --with-llvmsrc=${LLVM_ROOT}/llvm-2.8 --with-llvmobj=${LLVM_ROOT}/llvm-2.8/build

8. make
