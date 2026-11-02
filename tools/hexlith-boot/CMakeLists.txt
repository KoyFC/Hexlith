add_executable(hexlith-boot main.cpp)

target_link_libraries(hexlith-boot PRIVATE Hexlith::Adapters Hexlith::Vulkan Hexlith::Sdl Hexlith::Warnings)

# The lifted library is built apart from this binary and calls into it for the
# domain's code: the interpreter, guest memory and the primitives that are not
# inline. The symbols have to be visible to a library opened at run time.
set_target_properties(hexlith-boot PROPERTIES ENABLE_EXPORTS ON)
