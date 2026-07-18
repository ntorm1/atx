# Vendored Peter Jäckel "Let's Be Rational" — bench-only IV-inversion reference.
# See README.md / LICENSE. Third-party sources are compiled verbatim; ALL build
# adaptation lives here so the vendored files stay byte-clean (license: preserve
# the notice) and provenance-checkable.
#
# Isolated as a static lib so its warnings never reach the atx tree: it does NOT
# link atx_warnings, builds with warnings fully suppressed (upstream compiles
# with `-w -fpermissive`), and defines NOMINMAX so the vestigial <windows.h>
# include cannot leak min()/max() macros into std::max/std::min.

add_library(lbr_lets_be_rational STATIC
    LetsBeRational.cpp
    normaldistribution.cpp
    rationalcubic.cpp
    erf_cody.cpp
)

target_include_directories(lbr_lets_be_rational PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(lbr_lets_be_rational PRIVATE NOMINMAX NDEBUG)

# Suppress ALL warnings for the vendored code (f2c-translated Cody in particular
# trips many). Never promote them to errors here.
if(MSVC)
    target_compile_options(lbr_lets_be_rational PRIVATE /w)
else()
    target_compile_options(lbr_lets_be_rational PRIVATE -w)
endif()
