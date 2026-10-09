/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_program_proxy.h"
#include "pw_d3d9_api_observe.h"
#include <string.h>
struct program_proxy {
    const void *vtable;
    LONG references;
    IDirect3DDevice9 *parent;
    uint32_t kind;
    struct pw_d3d9_object_ref remote;
    struct pw_d3d9_deferred cleanup;
    struct program_proxy *next;
};
static SRWLOCK lock=SRWLOCK_INIT;
static struct program_proxy *programs;
static struct pw_d3d9_program_proxy_ops ops;
static void destroy_local(struct program_proxy *p)
{
    IDirect3DDevice9 *parent=p->parent;
    HeapFree(GetProcessHeap(),0,p);IDirect3DDevice9_Release(parent);
}
static void final_release(void *context)
{
    struct program_proxy *p=context;HRESULT hr=ops.release(p->parent,p->remote);
    if(hr==RPC_E_CANTCALLOUT_ININPUTSYNCCALL){
        p->cleanup.function=final_release;p->cleanup.context=p;
        hr=ops.defer(p->parent,&p->cleanup);
        if(FAILED(hr))ops.fail(p->parent,hr); /* Retain shell and parent on enqueue failure. */
        return;
    }
    if(FAILED(hr))ops.fail(p->parent,hr);
    destroy_local(p);
}
static ULONG addref(struct program_proxy *p){return (ULONG)InterlockedIncrement(&p->references);}
static ULONG release(struct program_proxy *p)
{
    AcquireSRWLockExclusive(&lock);ULONG refs=(ULONG)InterlockedDecrement(&p->references);
    if(!refs){struct program_proxy **link=&programs;while(*link!=p)link=&(*link)->next;*link=p->next;}
    ReleaseSRWLockExclusive(&lock);
    if(!refs)final_release(p);
    return refs;
}
static HRESULT query(struct program_proxy *p,REFIID iid,void **out)
{
    if(!out)return E_POINTER;
    *out=NULL;const GUID *typed=p->kind==7?&IID_IDirect3DVertexDeclaration9:p->kind==8?&IID_IDirect3DVertexShader9:&IID_IDirect3DPixelShader9;
    if(!IsEqualGUID(iid,&IID_IUnknown)&&!IsEqualGUID(iid,typed))return E_NOINTERFACE;
    addref(p);*out=p;return S_OK;
}
static HRESULT get_device(struct program_proxy *p,IDirect3DDevice9 **out)
{if(!out)return D3DERR_INVALIDCALL;*out=p->parent;IDirect3DDevice9_AddRef(*out);return S_OK;}
static HRESULT get_data(struct program_proxy *p,void *data,UINT *size)
{
    if(!size)return D3DERR_INVALIDCALL;
    addref(p);HRESULT hr=ops.query(p->parent,p->remote,p->kind,data,size);release(p);return hr;
}
#define METHODS(prefix,iface,datatype) \
static HRESULT WINAPI prefix##_query(iface *self,REFIID iid,void **out){return query((void *)self,iid,out);} \
static ULONG WINAPI prefix##_addref(iface *self){return addref((void *)self);} \
static ULONG WINAPI prefix##_release(iface *self){return release((void *)self);} \
static HRESULT WINAPI prefix##_device(iface *self,IDirect3DDevice9 **out){return get_device((void *)self,out);} \
static HRESULT WINAPI prefix##_data(iface *self,datatype *data,UINT *size){return get_data((void *)self,data,size);}
METHODS(decl,IDirect3DVertexDeclaration9,D3DVERTEXELEMENT9)
METHODS(vs,IDirect3DVertexShader9,void)
METHODS(ps,IDirect3DPixelShader9,void)
static const IDirect3DVertexDeclaration9Vtbl decl_vtable={decl_query,decl_addref,decl_release,decl_device,decl_data};
static const IDirect3DVertexShader9Vtbl vs_vtable={vs_query,vs_addref,vs_release,vs_device,vs_data};
static const IDirect3DPixelShader9Vtbl ps_vtable={ps_query,ps_addref,ps_release,ps_device,ps_data};
static struct program_proxy *allocate_shell(IDirect3DDevice9 *parent,uint32_t kind)
{
    struct program_proxy *p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));
    if(!p)return NULL;
    p->parent=parent;p->kind=kind;p->references=1;
    p->vtable=kind==7?(const void *)PW_D3D9_API_OBSERVE(IDirect3DVertexDeclaration9,&decl_vtable):
        kind==8?(const void *)PW_D3D9_API_OBSERVE(IDirect3DVertexShader9,&vs_vtable):
        (const void *)PW_D3D9_API_OBSERVE(IDirect3DPixelShader9,&ps_vtable);
    if(!p->vtable){HeapFree(GetProcessHeap(),0,p);return NULL;}
    IDirect3DDevice9_AddRef(parent); /* No COM calls under the cache lock. */
    return p;
}
static HRESULT publish_shell(struct program_proxy *shell,struct pw_d3d9_object_ref ref,void **out)
{
    shell->remote=ref;
    if(out)*out=NULL;
    if(!out||shell->kind<7||shell->kind>9||!ref.id||!ref.generation){
        if(ref.id&&ref.generation)final_release(shell);else destroy_local(shell);
        return D3DERR_INVALIDCALL;
    }
    struct program_proxy *p;
    AcquireSRWLockExclusive(&lock);
    for(p=programs;p;p=p->next)
        if(p->parent==shell->parent&&p->kind==shell->kind&&p->remote.id==ref.id&&p->remote.generation==ref.generation){addref(p);break;}
    if(!p){p=shell;p->next=programs;programs=p;}
    ReleaseSRWLockExclusive(&lock);
    if(p!=shell)final_release(shell);
    *out=p;return S_OK;
}
HRESULT pw_d3d9_program_proxy_wrap(IDirect3DDevice9 *parent,uint32_t kind,struct pw_d3d9_object_ref ref,void **out)
{
    if(out)*out=NULL;
    struct program_proxy *shell=allocate_shell(parent,kind);
    if(!shell){
        /* Cancellation owns the remote reference if no local cleanup record can be allocated. */
        ops.fail(parent,E_OUTOFMEMORY);return E_OUTOFMEMORY;
    }
    return publish_shell(shell,ref,out);
}
HRESULT pw_d3d9_program_proxy_resolve(IDirect3DDevice9 *parent,IUnknown *local,uint32_t kind,struct pw_d3d9_object_ref *out)
{
    if(!out)return E_POINTER;
    *out=(struct pw_d3d9_object_ref){0};if(!local)return S_OK;
    HRESULT hr=D3DERR_INVALIDCALL;AcquireSRWLockShared(&lock);
    for(struct program_proxy *p=programs;p;p=p->next)if((void *)p==(void *)local&&p->parent==parent&&p->kind==kind){*out=p->remote;hr=S_OK;break;}
    ReleaseSRWLockShared(&lock);return hr;
}
static int token(void *context,size_t index,uint32_t *value)
{
    uintptr_t base=(uintptr_t)context;SIZE_T got;
    if(index>SIZE_MAX/4||base>UINTPTR_MAX-index*4||base+index*4>UINTPTR_MAX-3)return 1;
    return !ReadProcessMemory(GetCurrentProcess(),(const void *)(base+index*4),value,4,&got)||got!=4;
}
static void put16(unsigned char *p,unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void put32(unsigned char *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static HRESULT create(IDirect3DDevice9 *parent,uint32_t kind,const void *input,void **out)
{
    if(!out)return D3DERR_INVALIDCALL;
    *out=NULL;if(!input)return D3DERR_INVALIDCALL;
    unsigned char *copy=NULL;size_t bytes=0;HRESULT hr=D3DERR_INVALIDCALL;
    if(kind==7){
        D3DVERTEXELEMENT9 elements[65];SIZE_T got;unsigned count;
        for(count=0;count<65;count++){
            uintptr_t base=(uintptr_t)input,offset=count*sizeof(elements[0]);if(base>UINTPTR_MAX-offset||base+offset>UINTPTR_MAX-sizeof(elements[0])+1)return hr;
            if(!ReadProcessMemory(GetCurrentProcess(),(const void *)(base+offset),elements+count,sizeof(elements[0]),&got)||got!=sizeof(elements[0]))return hr;
            if(elements[count].Stream==0xff){count++;break;}
        }
        if(!count||elements[count-1].Stream!=0xff)return hr;
        bytes=count*8;copy=HeapAlloc(GetProcessHeap(),0,bytes);if(!copy)return E_OUTOFMEMORY;
        for(unsigned n=0;n<count;n++){unsigned char *p=copy+n*8;put16(p,elements[n].Stream);put16(p+2,elements[n].Offset);p[4]=elements[n].Type;p[5]=elements[n].Method;p[6]=elements[n].Usage;p[7]=elements[n].UsageIndex;}
    }else{
        if(pw_d3d9_program_measure(kind,token,(void *)input,PW_D3D9_PROGRAM_LIMIT/4,&bytes)!=PW_D3D9_PROGRAM_OK)return hr;
        bytes*=4;copy=HeapAlloc(GetProcessHeap(),0,bytes);if(!copy)return E_OUTOFMEMORY;
        for(size_t n=0;n<bytes/4;n++){uint32_t value;if(token((void *)input,n,&value)){HeapFree(GetProcessHeap(),0,copy);return hr;}put32(copy+n*4,value);}
    }
    if(pw_d3d9_program_validate(kind,copy,bytes)!=PW_D3D9_PROGRAM_OK){HeapFree(GetProcessHeap(),0,copy);return hr;}
    struct program_proxy *shell=allocate_shell(parent,kind);
    if(!shell){HeapFree(GetProcessHeap(),0,copy);return E_OUTOFMEMORY;}
    struct pw_d3d9_program_request q={.operation=PW_D3D9_PROGRAM_BEGIN,.kind=kind,.total=(uint32_t)bytes};struct pw_d3d9_program_reply r;
    uint64_t transfer=0;hr=ops.program(parent,&q,&r);if(FAILED(hr))goto done;transfer=r.transfer;
    q=(struct pw_d3d9_program_request){.operation=PW_D3D9_PROGRAM_WRITE,.kind=kind,.transfer=transfer};
    for(size_t offset=0;offset<bytes;offset+=q.count){q.offset=(uint32_t)offset;q.count=(uint32_t)(bytes-offset>4096?4096:bytes-offset);memcpy(q.data,copy+offset,q.count);hr=ops.program(parent,&q,&r);if(FAILED(hr))goto abort;}
    q=(struct pw_d3d9_program_request){.operation=PW_D3D9_PROGRAM_COMMIT,.kind=kind,.transfer=transfer};hr=ops.program(parent,&q,&r);
    if(SUCCEEDED(hr)){HRESULT published=publish_shell(shell,(struct pw_d3d9_object_ref){r.id,r.generation},out);shell=NULL;if(FAILED(published))hr=published;}
    else goto abort;
    goto done;
 abort:
    q=(struct pw_d3d9_program_request){.operation=PW_D3D9_PROGRAM_ABORT,.kind=kind,.transfer=transfer};
    {HRESULT cleanup=ops.program(parent,&q,&r);if(FAILED(cleanup)&&cleanup!=D3DERR_INVALIDCALL)ops.fail(parent,cleanup);}
 done:
    HeapFree(GetProcessHeap(),0,copy);if(shell)destroy_local(shell);return hr;
}
static HRESULT WINAPI create_decl(IDirect3DDevice9 *d,const D3DVERTEXELEMENT9 *data,IDirect3DVertexDeclaration9 **out){return create(d,7,data,(void **)out);}
static HRESULT WINAPI create_vs(IDirect3DDevice9 *d,const DWORD *data,IDirect3DVertexShader9 **out){return create(d,8,data,(void **)out);}
static HRESULT WINAPI create_ps(IDirect3DDevice9 *d,const DWORD *data,IDirect3DPixelShader9 **out){return create(d,9,data,(void **)out);}
void pw_d3d9_program_proxy_install(IDirect3DDevice9Vtbl *vtable,const struct pw_d3d9_program_proxy_ops *callbacks)
{ops=*callbacks;vtable->CreateVertexDeclaration=create_decl;vtable->CreateVertexShader=create_vs;vtable->CreatePixelShader=create_ps;}
