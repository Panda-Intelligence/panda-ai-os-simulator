mergeInto(LibraryManager.library, {
  $PANDA_PTY_TIMEVAL_LAYOUT: {
    sec: {{{ C_STRUCTS.timeval.tv_sec }}},
    usec: {{{ C_STRUCTS.timeval.tv_usec }}},
    size: {{{ C_STRUCTS.timeval.__size__ }}},
  },
});
