cmake_minimum_required (VERSION 3.5)

install (
    FILES adu-agent.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
    PERMISSIONS
        OWNER_READ
        OWNER_WRITE
        GROUP_READ
        WORLD_READ)

install (
    FILES adu-agent-setup.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
    PERMISSIONS
        OWNER_READ
        OWNER_WRITE
        GROUP_READ
        WORLD_READ)

install (
    FILES adu-agent-setup.sh
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
    PERMISSIONS
        OWNER_READ
        OWNER_WRITE
        OWNER_EXECUTE)
