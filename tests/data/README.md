# Archive test data

Used by `tests/archive_test.cpp`. The same three files in each archive:

- `Game One.sfc`: 100000 bytes, byte i = (i * 7) % 251
- `sub/Gâme Two.md`: "second file\n" 50 times (a non-ASCII name in a folder)
- `empty.txt`: empty

Made with 7-Zip 26.00, from a folder holding those files:

```sh
7z a -t7z -m0=lzma2 -ms=on -mtm=off -mtc=off -mta=off lzma2_solid.7z 'Game One.sfc' sub empty.txt
7z a -t7z -m0=PPMd -mtm=off -mtc=off -mta=off ppmd.7z 'Game One.sfc' sub empty.txt
```

`files.zip` holds the same three files plus an explicit `sub/` folder entry and
`win\file.txt` (a Windows separator stored in the name), made with Python's
zipfile (deflate; UTF-8 name flag set for the non-ASCII name).

`ppmd.7z` checks that PPMd, which isn't built, is refused cleanly.
