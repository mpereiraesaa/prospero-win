/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
int wmain(int argc,WCHAR **argv)
{
    if(argc!=4)return 2;
    if(!SetEnvironmentVariableW(L"PW_D3D9_SERVICE64",argv[2])||
       !SetEnvironmentVariableW(L"PW_D3D9_BACKEND64",argv[3]))return 3;
    HMODULE module=LoadLibraryW(argv[1]);
    IDirect3D9 *(WINAPI *create)(UINT)=module?(void *)GetProcAddress(module,"Direct3DCreate9"):NULL;
    if(!create)return 4;
    IDirect3D9 *factory=create(D3D_SDK_VERSION);
    if(factory){IDirect3D9_Release(factory);FreeLibrary(module);return 5;}
    puts("PW_PIPELINE_MIXED factory_null=1 no_published_object=1 status=0");
    FreeLibrary(module);return 0;
}
