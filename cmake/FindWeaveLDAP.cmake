find_path(WeaveLDAP_INCLUDE_DIR NAMES ldap.h)
find_library(WeaveLDAP_LIBRARY NAMES ldap libldap.so.2)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WeaveLDAP REQUIRED_VARS WeaveLDAP_INCLUDE_DIR WeaveLDAP_LIBRARY)
if(WeaveLDAP_FOUND AND NOT TARGET WeaveLDAP::LDAP)
  add_library(WeaveLDAP::LDAP UNKNOWN IMPORTED)
  set_target_properties(WeaveLDAP::LDAP PROPERTIES
    IMPORTED_LOCATION "${WeaveLDAP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WeaveLDAP_INCLUDE_DIR}")
endif()
mark_as_advanced(WeaveLDAP_INCLUDE_DIR WeaveLDAP_LIBRARY)
