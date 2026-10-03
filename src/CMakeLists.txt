# The include path is this directory, so a header is included by its layer:
# #include "core/result.h", #include "domain/ctr/ncch.h". The include line
# then names the layer, which is what makes a dependency pointing the wrong
# way visible in a diff.
add_subdirectory(core)
add_subdirectory(application)
