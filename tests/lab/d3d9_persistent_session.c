/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include <d3d9.h>
#include <stdio.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line=%d\n",__LINE__);return 1;} } while(0)
struct caller {struct pw_d3d9_session *session;struct pw_d3d9_object_ref ref;};
static DWORD WINAPI concurrent_calls(void *arg)
{
    struct caller *c=arg;
    struct pw_d3d9_factory_request q={.method=4};struct pw_d3d9_factory_reply r;
    for(unsigned n=0;n<100;n++)
        if(pw_d3d9_session_factory(c->session,c->ref,&q,&r)!=S_OK||!r.count)return 1;
    return 0;
}
int wmain(int argc,WCHAR **argv)
{
    struct pw_d3d9_session *s=NULL,*other=NULL;
    struct pw_d3d9_object_ref a,b,c;
    struct pw_d3d9_factory_request q={0};struct pw_d3d9_factory_reply r;
    CHECK(argc==3);
    CHECK(FAILED(pw_d3d9_session_open(L"Z:\\missing-service.dll",argv[2],&s))&&!s);
    CHECK(FAILED(pw_d3d9_session_open(argv[1],L"Z:\\missing-backend.dll",&s))&&!s);
    for(unsigned cycle=0;cycle<3;cycle++){
        CHECK(pw_d3d9_session_open(argv[1],argv[2],&s)==S_OK&&s);
        CHECK(FAILED(pw_d3d9_session_open(argv[1],argv[2],&other))&&!other);
        CHECK(pw_d3d9_session_create(s,D3D_SDK_VERSION,&a)==S_OK);
        CHECK(pw_d3d9_session_create(s,D3D_SDK_VERSION,&b)==S_OK);
        q=(struct pw_d3d9_factory_request){.method=4};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK&&r.count>0);
        unsigned adapters=r.count;
        q=(struct pw_d3d9_factory_request){.method=5};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK&&r.identifier.description[0]);
        q=(struct pw_d3d9_factory_request){.method=14,.device_type=D3DDEVTYPE_HAL};
        CHECK(pw_d3d9_session_factory(s,b,&q,&r)==S_OK);
        q.adapter=0xffffffffu;CHECK(pw_d3d9_session_factory(s,b,&q,&r)==D3DERR_INVALIDCALL);
        q.method=16;CHECK(pw_d3d9_session_factory(s,b,&q,&r)==E_NOTIMPL);
        q=(struct pw_d3d9_factory_request){.method=8};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK&&r.mode.width&&r.mode.height);
        unsigned format=r.mode.format;
        q=(struct pw_d3d9_factory_request){.method=6,.format=format};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK&&r.count);
        q.method=7;CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK&&r.mode.width);
        q=(struct pw_d3d9_factory_request){.method=9,.device_type=D3DDEVTYPE_HAL,.format=format,.format2=format,.windowed=1};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK);
        q=(struct pw_d3d9_factory_request){.method=10,.device_type=D3DDEVTYPE_HAL,.format=format,.format2=D3DFMT_A8R8G8B8,.resource_type=D3DRTYPE_TEXTURE};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK);
        q=(struct pw_d3d9_factory_request){.method=11,.device_type=D3DDEVTYPE_HAL,.format=format,.windowed=1,.multisample=D3DMULTISAMPLE_NONE};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK);
        q=(struct pw_d3d9_factory_request){.method=12,.device_type=D3DDEVTYPE_HAL,.format=format,.format2=format,.format3=D3DFMT_D24S8};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK);
        q=(struct pw_d3d9_factory_request){.method=13,.device_type=D3DDEVTYPE_HAL,.format=format,.format2=format};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==S_OK);
        struct caller caller={s,a};HANDLE threads[4];
        for(unsigned n=0;n<4;n++){threads[n]=CreateThread(NULL,0,concurrent_calls,&caller,0,NULL);CHECK(threads[n]);}
        CHECK(WaitForMultipleObjects(4,threads,TRUE,30000)==WAIT_OBJECT_0);
        for(unsigned n=0;n<4;n++){DWORD code=1;CHECK(GetExitCodeThread(threads[n],&code)&&!code);CloseHandle(threads[n]);}
        CHECK(pw_d3d9_session_release(s,a)==S_OK);
        q=(struct pw_d3d9_factory_request){.method=14,.device_type=D3DDEVTYPE_HAL};
        CHECK(pw_d3d9_session_factory(s,a,&q,&r)==D3DERR_INVALIDCALL);
        CHECK(pw_d3d9_session_create(s,D3D_SDK_VERSION,&c)==S_OK);
        CHECK(c.id!=a.id||c.generation!=a.generation);
        CHECK(pw_d3d9_session_release(s,b)==S_OK);
        /* Leave c live: STOP must release it before unloading the backend. */
        CHECK(pw_d3d9_session_close(s)==S_OK);s=NULL;
        printf("PW_PERSISTENT_SESSION cycle=%u adapters=%u status=0\n",cycle,adapters);fflush(stdout);
    }
    CHECK(pw_d3d9_session_open(argv[1],argv[2],&s)==S_OK);
    CHECK(pw_d3d9_session_create(s,D3D_SDK_VERSION,&a)==S_OK);
    pw_d3d9_session_cancel(s);
    CHECK(FAILED(pw_d3d9_session_close(s)));
    CHECK(pw_d3d9_session_open(argv[1],argv[2],&s)==S_OK);
    CHECK(pw_d3d9_session_close(s)==S_OK);
    puts("PW_PERSISTENT_SESSION cancellation=recovered calls=1200");
    return 0;
}
