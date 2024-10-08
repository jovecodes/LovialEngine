set -e
# gcc ./test.c -Ilua/src -Llua/src -llua -lm -o test

# bear -- g++  -o main ./main.cpp -L. -ljovial_engine -I../NewJovialEngine/src -Ilua/src -Llua/src -llua  -ggdb

g++ -o editor/editor ./editor/editor.cpp -L. -ljovial_engine -I../NewJovialEngine/src -Ilua/src -Llua/src -llua -ggdb -lfreetype -fsanitize=address,undefined
./editor/editor
