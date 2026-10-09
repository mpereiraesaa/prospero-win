/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/pw_d3d9_window_driver.h"
#include <d3d9.h>
#include <stdio.h>
static LRESULT CALLBACK window_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam)
{return DefWindowProcW(hwnd,message,wparam,lparam);}
int wmain(int argc,WCHAR **argv)
{
    if(argc!=4&&argc!=5)return 2;
    int unavailable=argc==5;if(unavailable&&wcscmp(argv[4],L"--expect-unavailable"))return 2;
    if(!SetEnvironmentVariableW(L"PW_D3D9_SERVICE64",argv[2])||!SetEnvironmentVariableW(L"PW_D3D9_BACKEND64",argv[3]))return 3;
    HMODULE proxy=LoadLibraryW(argv[1]);if(!proxy)return 4;
    IDirect3D9 *(WINAPI *create9)(UINT)=(void *)GetProcAddress(proxy,"Direct3DCreate9");if(!create9)return 5;
    WNDCLASSEXW cls={.cbSize=sizeof(cls),.lpfnWndProc=window_proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_COM_DEVICE_GUEST"};
    if(!RegisterClassExW(&cls))return 6;
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"D3D9 owner COM device",WS_OVERLAPPEDWINDOW|WS_VISIBLE,20,20,640,480,NULL,NULL,cls.hInstance,NULL);
    if(!window)return 7;
    for(unsigned cycle=0;cycle<3;cycle++){
        IDirect3D9 *factory=create9(D3D_SDK_VERSION);if(!factory)return 8;
        IDirect3DDevice9 *device=NULL;D3DPRESENT_PARAMETERS pp={.BackBufferFormat=D3DFMT_UNKNOWN,.BackBufferCount=1,.SwapEffect=D3DSWAPEFFECT_DISCARD,.hDeviceWindow=window,.Windowed=TRUE,.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE};
        HRESULT create=IDirect3D9_CreateDevice(factory,0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device),reset=E_FAIL,present=E_FAIL;
        if(unavailable){if(create!=D3DERR_NOTAVAILABLE||device)return 9;}
        else{
            if(FAILED(create)||!device||!pp.BackBufferWidth||!pp.BackBufferHeight)return 10;
            IUnknown *identity=NULL;IDirect3D9 *parent=NULL;D3DDEVICE_CREATION_PARAMETERS creation;
            if(FAILED(IDirect3DDevice9_QueryInterface(device,&IID_IUnknown,(void **)&identity))||identity!=(IUnknown *)device)return 11;
            IUnknown_Release(identity);
            if(FAILED(IDirect3DDevice9_GetDirect3D(device,&parent))||parent!=factory)return 12;
            IDirect3D9_Release(parent);
            if(FAILED(IDirect3DDevice9_GetCreationParameters(device,&creation))||creation.hFocusWindow!=window)return 13;
            reset=IDirect3DDevice9_Reset(device,&pp);present=IDirect3DDevice9_Present(device,NULL,NULL,NULL,NULL);
            if(FAILED(reset)||FAILED(present))return 14;
            if(IDirect3DDevice9_Release(device))return 15;
        }
        if(IDirect3D9_Release(factory))return 16;
        printf("PW_DEVICE_PROXY cycle=%u owner=1 create=%08lx reset=%08lx present=%08lx unavailable=%d status=0\n",cycle,(DWORD)create,(DWORD)reset,(DWORD)present,unavailable);fflush(stdout);
    }
    DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(proxy);return 0;
}
