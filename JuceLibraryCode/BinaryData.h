/* =========================================================================================

   This is an auto-generated file: Any edits you make may be overwritten!

*/

#pragma once

namespace BinaryData
{
    extern const char*   LonePearLogic_png;
    const int            LonePearLogic_pngSize = 331159;

    extern const char*   MuteOFF_png;
    const int            MuteOFF_pngSize = 9443;

    extern const char*   MuteON_png;
    const int            MuteON_pngSize = 9965;

    extern const char*   SlotMachineUserManual_html;
    const int            SlotMachineUserManual_htmlSize = 488819;

    extern const char*   SlotMachine_ico;
    const int            SlotMachine_icoSize = 4286;

    extern const char*   SM5_png;
    const int            SM5_pngSize = 50997;

    extern const char*   SoloOFF_png;
    const int            SoloOFF_pngSize = 9443;

    extern const char*   SoloON_png;
    const int            SoloON_pngSize = 9950;

    // Number of elements in the namedResourceList and originalFileNames arrays.
    const int namedResourceListSize = 8;

    // Points to the start of a list of resource names.
    extern const char* namedResourceList[];

    // Points to the start of a list of resource filenames.
    extern const char* originalFilenames[];

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding data and its size (or a null pointer if the name isn't found).
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);

    // If you provide the name of one of the binary resource variables above, this function will
    // return the corresponding original, non-mangled filename (or a null pointer if the name isn't found).
    const char* getNamedResourceOriginalFilename (const char* resourceNameUTF8);
}
