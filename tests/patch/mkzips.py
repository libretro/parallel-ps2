# The archives pnachload reads, written with Python's zipfile so the
# reader is checked against a writer it does not share code with.
import sys, zipfile, os
out = sys.argv[1]
def z(name, members, comment=b"", method=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(os.path.join(out, name), "w", method) as f:
        for n, t in members:
            f.writestr(n, t)
        f.comment = comment
z("deflated.zip", [("readme.txt", "x" * 300),
                   ("ABCD1234.pnach", "patch=1,EE,00000008,word,8\n" + "// padding\n" * 40 + "patch=1,EE,00000009,word,9\n")])
z("stored.zip", [("ABCD1234.PNACH", "patch=1,EE,0000000a,word,10\n")], method=zipfile.ZIP_STORED)
z("subdir.zip", [("sub/ABCD1234.pnach", "patch=1,EE,0000000c,word,12\n")])
z("comment.zip", [("ABCD1234.pnach", "patch=1,EE,0000000b,word,11\n")], comment=b"a comment, " * 20)
