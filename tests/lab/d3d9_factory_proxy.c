/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line=%d\n",__LINE__);return 1;}} while(0)
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==4);
    for(unsigned arg=2;arg<=3;arg++)for(WCHAR *p=argv[arg];*p;p++)if(*p==L'\\')*p=L'/';
    CHECK(SetEnvironmentVariableW(L"PW_D3D9_SERVICE64",argv[2]));
    CHECK(SetEnvironmentVariableW(L"PW_D3D9_BACKEND64",argv[3]));
    HMODULE module=LoadLibraryW(argv[1]);CHECK(module);
    IDirect3D9 *(WINAPI *create)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");
    HRESULT (WINAPI *create_ex)(UINT,IDirect3D9Ex **)=(void *)GetProcAddress(module,"Direct3DCreate9Ex");
    CHECK(create&&create_ex);
    IDirect3D9Ex *ex=(void *)1;CHECK(create_ex(D3D_SDK_VERSION,&ex)==D3DERR_NOTAVAILABLE&&!ex);
    for(unsigned cycle=0;cycle<3;cycle++){
        IDirect3D9 *a=create(D3D_SDK_VERSION),*b=create(D3D_SDK_VERSION);CHECK(a&&b);
        IUnknown *identity=NULL;IDirect3D9 *again=NULL;
        CHECK(IDirect3D9_QueryInterface(a,&IID_IUnknown,(void **)&identity)==S_OK&&identity==(IUnknown *)a);
        CHECK(IUnknown_QueryInterface(identity,&IID_IDirect3D9,(void **)&again)==S_OK&&again==a);
        IUnknown_Release(identity);IDirect3D9_Release(again);
        ex=(void *)1;CHECK(IDirect3D9_QueryInterface(a,&IID_IDirect3D9Ex,(void **)&ex)==E_NOINTERFACE&&!ex);
        CHECK(IDirect3D9_GetAdapterCount(a)>0);
        D3DADAPTER_IDENTIFIER9 id;CHECK(IDirect3D9_GetAdapterIdentifier(a,0,0,&id)==S_OK&&id.Description[0]);
        D3DDISPLAYMODE mode;CHECK(IDirect3D9_GetAdapterDisplayMode(a,0,&mode)==S_OK);
        CHECK(IDirect3D9_GetAdapterModeCount(a,0,mode.Format)>0);
        CHECK(IDirect3D9_EnumAdapterModes(a,0,mode.Format,0,&mode)==S_OK);
        CHECK(IDirect3D9_CheckDeviceType(a,0,D3DDEVTYPE_HAL,mode.Format,mode.Format,TRUE)==S_OK);
        CHECK(IDirect3D9_CheckDeviceFormat(a,0,D3DDEVTYPE_HAL,mode.Format,0,D3DRTYPE_TEXTURE,D3DFMT_A8R8G8B8)==S_OK);
        DWORD quality;CHECK(IDirect3D9_CheckDeviceMultiSampleType(a,0,D3DDEVTYPE_HAL,mode.Format,TRUE,D3DMULTISAMPLE_NONE,&quality)==S_OK);
        CHECK(IDirect3D9_CheckDepthStencilMatch(a,0,D3DDEVTYPE_HAL,mode.Format,mode.Format,D3DFMT_D24S8)==S_OK);
        CHECK(IDirect3D9_CheckDeviceFormatConversion(a,0,D3DDEVTYPE_HAL,mode.Format,mode.Format)==S_OK);
        D3DCAPS9 caps;CHECK(IDirect3D9_GetDeviceCaps(a,0,D3DDEVTYPE_HAL,&caps)==S_OK&&caps.MaxTextureWidth>0);
        memset(&caps,0xa5,sizeof(caps));D3DCAPS9 unchanged=caps;
        CHECK(IDirect3D9_GetDeviceCaps(a,0xffffffff,D3DDEVTYPE_HAL,&caps)==D3DERR_INVALIDCALL&&!memcmp(&caps,&unchanged,sizeof(caps)));
        CHECK(IDirect3D9_GetDeviceCaps(a,0,D3DDEVTYPE_HAL,NULL)==D3DERR_INVALIDCALL);
        CHECK(IDirect3D9_RegisterSoftwareDevice(a,NULL)==D3DERR_NOTAVAILABLE);
        HMONITOR monitor=IDirect3D9_GetAdapterMonitor(a,0);MONITORINFOEXA info={.cbSize=sizeof(info)};
        CHECK(monitor&&GetMonitorInfoA(monitor,(MONITORINFO *)&info)&&!lstrcmpiA(info.szDevice,id.DeviceName));
        CHECK(!IDirect3D9_GetAdapterMonitor(a,0xffffffff));
        IDirect3DDevice9 *device=(void *)1;
#ifdef PW_D3D9_ENABLE_DEVICE
        CHECK(IDirect3D9_CreateDevice(a,0,D3DDEVTYPE_HAL,NULL,0,NULL,&device)==D3DERR_INVALIDCALL&&!device);
#else
        CHECK(IDirect3D9_CreateDevice(a,0,D3DDEVTYPE_HAL,NULL,0,NULL,&device)==D3DERR_NOTAVAILABLE&&!device);
#endif
        CHECK(IDirect3D9_Release(a)==0);
        CHECK(IDirect3D9_GetAdapterCount(b)>0);
        CHECK(IDirect3D9_Release(b)==0);
        printf("PW_FACTORY_PROXY cycle=%u identity=1 methods=12 unsupported=3 status=0\n",cycle);fflush(stdout);
    }
    CHECK(FreeLibrary(module));return 0;
}
