# Copyright 2015-present Facebook. All Rights Reserved.
lib: libbic.so

libbic.so: bic.c
	$(CC) $(CFLAGS) -fPIC -c -o bic.o bic.c
	$(CC) -lipmb -ledb -lkv -shared -o libbic.so bic.o -lc 

.PHONY: clean

clean:
	rm -rf *.o libbic.so
