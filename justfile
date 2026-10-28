# The backend is a compile-time choice, so every recipe reconfigures and runs both.
_default: test example

# SystemC: xmake test (category 1). SVDPI: xmake run vcs-test (categories 2-4, skips without VCS).
test:
  xmake f --backend=systemc && xmake test
  xmake f --backend=svdpi && xmake run vcs-test

# Same documented example output per backend.
example:
  xmake f --backend=systemc && xmake run example
  xmake f --backend=svdpi && xmake run example
