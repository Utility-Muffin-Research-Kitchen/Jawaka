## Catastrophe submodule

Once Jawaka has a git origin configured, add Catastrophe as a submodule:

```sh
git submodule add <CATASTROPHE_REPO_URL> third_party/catastrophe
git submodule update --init --recursive
```

Until then, developers should point the Makefile at a local checkout by
exporting `CATASTROPHE_DIR=/path/to/Catastrophe` before invoking `make`.
Phase 0+1 already uses Catastrophe for minimal launcher/menu surfaces, so
`CATASTROPHE_DIR` is part of the expected developer setup until the submodule
can be added for real.
