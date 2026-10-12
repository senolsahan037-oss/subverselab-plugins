# SubverseLab desktop-app icon (docs/APP_ICON_STANDARD.md).
#
#   include(<plugin-common>/cmake/SvlAppIcon.cmake)
#   svl_app_icon(<juce target> <icon folder made by tools/app_icon/make_app_icon.py>)
#
# After every build of <target>_Standalone: copies compiled/Assets.car and compiled/AppIcon.icns into the app, sets
# CFBundleIconName = AppIcon (without it macOS 26+ ignores Assets.car and puts the icon on a grey plate), and re-signs the bundle
# ad hoc, because editing a signed bundle invalidates its signature (the release signing step signs it again with the real identity).
function(svl_app_icon target icon_dir)
    if(NOT APPLE OR NOT TARGET ${target}_Standalone)
        return()
    endif()
    set(car  "${icon_dir}/compiled/Assets.car")
    set(icns "${icon_dir}/compiled/AppIcon.icns")
    if(NOT EXISTS "${car}")
        message(WARNING "svl_app_icon: ${car} missing; run tools/app_icon/make_app_icon.py first")
        return()
    endif()
    add_custom_command(TARGET ${target}_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${car}" "${icns}" "$<TARGET_BUNDLE_CONTENT_DIR:${target}_Standalone>/Resources/"
        COMMAND plutil -replace CFBundleIconName -string AppIcon "$<TARGET_BUNDLE_CONTENT_DIR:${target}_Standalone>/Info.plist"
        COMMAND plutil -replace CFBundleIconFile -string AppIcon "$<TARGET_BUNDLE_CONTENT_DIR:${target}_Standalone>/Info.plist"
        COMMAND codesign --force --deep -s - "$<TARGET_BUNDLE_DIR:${target}_Standalone>"
        COMMENT "SubverseLab app icon (Assets.car) -> ${target}_Standalone"
        VERBATIM)
endfunction()
