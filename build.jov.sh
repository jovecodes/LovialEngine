# gcc ./test.c -Ilua/src -Llua/src -llua -lm -o test
#
bear -- g++  -o main ./main.cpp -L. -ljovial_engine -I../NewJovialEngine/src -Ilua/src -Llua/src -llua  -ggdb
#
# g++ ./main.cpp -Ilua/src -Llua/src -llua -L. -ljovial_engine -I../NewJovialEngine/src -ggdb -o main -Wl,--trace
