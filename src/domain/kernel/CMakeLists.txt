target_sources(
    hexlith_domain
    PRIVATE address_space.cpp
            address_space.h
            executable_loader.cpp
            executable_loader.h
            handle_table.cpp
            handle_table.h
            objects.cpp
            objects.h
            region_allocator.cpp
            region_allocator.h
            result_code.h
            scheduler.cpp
            scheduler.h
            system_pages.cpp
            system_pages.h
)
