#!/bin/zsh
# Builds the portable core with miniz and converts every book in test/books.
set -e
cd "$(dirname "$0")"
mkdir -p out
if [ ! -f miniz/miniz.c ]; then  # miniz (MIT) provides tinfl on the PC; the ESP32-S3 has it in ROM
  curl -sSLf -o miniz.zip https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  unzip -o -q miniz.zip -d miniz
fi
clang -O1 -g -fsanitize=address,undefined -c miniz/miniz.c -o out/miniz.o
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -Iminiz -I../src/core \
  host_test.cpp ../src/core/book.cpp ../src/core/text.cpp ../src/core/rsvp.cpp out/miniz.o -o out/host_test
for b in books/*; do
  echo "===== $b"
  ./out/host_test "$b" "out/$(basename $b).clean.txt"
done
