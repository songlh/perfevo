perfevo
=======

static checkers for performance bugs

How to build?
1. edit ./autoconf/configure.ac
   a. LLVM_SRC_ROOT=${llvm_directory}               //directory to llvm source code 
   b. LLVM_OBJ_ROOT=${llvm_building_directory}      //directory to where llvm is built

2. cd ./autoconf

3. ./AutoRegen.sh        //provide llvm directory and llvm-building directory when running this command

4. cd ..

5. mkdir build

6. cd build

7. ../configure --with-llvmsrc=${llvm_directory} --with-llvmobj=${llvm_building_directory}

8. make
