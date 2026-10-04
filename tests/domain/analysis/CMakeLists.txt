hexlith_add_test(
    hexlith_domain_analysis_test provenance_test.cpp record_text_test.cpp tables_test.cpp
)

target_link_libraries(hexlith_domain_analysis_test PRIVATE Hexlith::Domain)
