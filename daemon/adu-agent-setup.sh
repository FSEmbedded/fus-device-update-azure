#!/bin/sh
AducIotAgent --update-type "fus/firmware:1" -C /var/lib/adu/extensions/sources/libfus_firmware_1.so
AducIotAgent --update-type "fus/application:1" -C /var/lib/adu/extensions/sources/libfus_application_1.so
AducIotAgent --update-type "microsoft/update-manifest" -C /var/lib/adu/extensions/sources/libmicrosoft_steps_1.so
AducIotAgent --update-type "microsoft/update-manifest:4" -C /var/lib/adu/extensions/sources/libmicrosoft_steps_1.so
AducIotAgent -D /var/lib/adu/extensions/sources/libdeliveryoptimization-content-downloader.so
