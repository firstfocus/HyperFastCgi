#include <memory.h>
#include <mono/metadata/object.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/class.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/image.h>
#include "mono-bridge.h"
#include "fcgi-transport.h"
#include "libev.h"

typedef void (*Host_CreateRequest)(MonoObject *obj, guint64 requestId, int request_num, MonoException** ex);
typedef void (*Host_AddServerVariable)(MonoObject *obj, guint64 requestId, int request_num,
                                        MonoString *name, MonoString *value, MonoException** ex);
typedef void (*Host_AddHeader)(MonoObject *obj, guint64 requestId, int request_num,
                                        MonoString *name, MonoString *value, MonoException** ex);
typedef void (*Host_HeadersSent)(MonoObject *obj, guint64 requestId, int request_num, MonoException** ex);
typedef void (*Host_AddBodyPart)(MonoObject *obj, guint64 requestId, int request_num,
                                        MonoArray *body, MonoBoolean final, MonoException** ex);
typedef void (*Host_Process)(MonoObject *obj, guint64 requestId, int request_num,
                                        MonoException** ex);


Host_CreateRequest host_create_request;
Host_AddServerVariable host_add_server_variable;
Host_AddHeader host_add_header;
Host_HeadersSent host_headers_sent;
Host_AddBodyPart host_add_body_part;
Host_Process host_process;

HostInfo app;

void
register_host (MonoObject* host, MonoString *virtual_path, MonoString *path)
{
//    MONO_OBJECT_SETREF(&app,host,host);
    //app.host = host;
    //mono_gc_wbarrier_generic_store(&app.host,host);
    app.host = mono_gchandle_get_target(mono_gchandle_new(host,TRUE));
    app.virtual_path = virtual_path;
    app.path = path;
}

HostInfo *
find_host_by_path (gchar* vpath)
{
    return &app;
}

void
create_request (HostInfo *host, guint64 requestId, int request_num)
{
    MonoException* ex = NULL;
    MonoDomain* domain=mono_object_get_domain(host->host);
    MonoDomain* current=mono_domain_get();
    mono_domain_set(domain,FALSE);
    host_create_request(host->host,requestId, request_num, &ex);
    //TODO: handle exception
    mono_domain_set(current,FALSE);
    if (ex)
        ERROR_OUT("exception! %s","add_server_variable");
}

void
add_server_variable (HostInfo *host, guint64 requestId, int request_num, guint8 *name, int name_len, guint8 *value, int value_len)
{
    MonoException* ex = NULL;
    MonoDomain* domain=mono_object_get_domain(host->host);
    MonoDomain* current=mono_domain_get();
    mono_domain_set(domain,FALSE);

    host_add_server_variable(host->host, requestId, request_num,
                            mono_string_new_len(domain, (const char *)name, name_len),
                            mono_string_new_len(domain, (const char *)value, value_len),
                            &ex);
    //TODO: handle exception
    mono_domain_set(current,FALSE);
    if (ex)
        ERROR_OUT("exception! %s","add_server_variable");
}

void
add_header (HostInfo *host, guint64 requestId, int request_num, guint8 *name, int name_len, guint8 *value, int value_len)
{
    MonoException* ex;
    MonoDomain* domain=mono_object_get_domain(host->host);
    MonoDomain* current=mono_domain_get();
    mono_domain_set(domain,FALSE);

    host_add_header(host->host, requestId, request_num,
                            mono_string_new_len(domain, (const char *)name, name_len),
                            mono_string_new_len(domain, (const char *)value, value_len),
                            &ex);
    //TODO: handle exception
    mono_domain_set(current,FALSE);

}

void
headers_sent (HostInfo *host, guint64 requestId, int request_num)
{
    MonoException* ex;
    MonoDomain* domain=mono_object_get_domain(host->host);
    MonoDomain* current=mono_domain_get();
    mono_domain_set(domain,FALSE);

    host_headers_sent(host->host, requestId, request_num,
                            &ex);
    //TODO: handle exception
    mono_domain_set(current,FALSE);
}

void
add_body_part (HostInfo *host, guint64 requestId, int request_num, guint8 *body, int len, gboolean final)
{
    MonoException *ex;
    MonoArray *byte_array = NULL;
    MonoClass *byte_class = mono_get_byte_class();
    MonoDomain *domain=mono_object_get_domain(host->host);
    MonoDomain *current=mono_domain_get();
    mono_domain_set(domain,FALSE);

    if (len > 0) {
        byte_array = mono_array_new(domain, byte_class, len);
        int elem_size = mono_class_data_size(byte_class);
        void *dest_addr = mono_array_addr_with_size(byte_array, elem_size, 0);
        memcpy(dest_addr,body,len);
    }

    host_add_body_part(host->host, requestId, request_num,
                            byte_array, final, &ex);
    //TODO: handle exception
    mono_domain_set(current,FALSE);
}

void
process (HostInfo *host, guint64 requestId, int request_num)
{
    MonoException* ex;
    MonoDomain* domain=mono_object_get_domain(host->host);
    MonoDomain* current=mono_domain_get();
    mono_domain_set(domain,FALSE);

    host_process(host->host, requestId, request_num,&ex);
    //TODO: handle exception
    mono_domain_set(current,FALSE);
}

void
bridge_send_output(MonoObject *transport, guint64 requestId, int request_num, MonoArray *byte_array, int len)
{
    MonoClass *byte_class = mono_get_byte_class();

    if (len > 0) {
        int elem_size = mono_class_data_size(byte_class);
        guint8 *body = (guint8 *)mono_array_addr_with_size(byte_array, elem_size, 0);
        send_output(requestId, request_num, body, len);
    }
}

void
bridge_end_request(MonoObject *transport, guint64 requestId, int request_num, int status)
{
    end_request(requestId, request_num, status, FCGI_REQUEST_COMPLETE);
}

typedef struct {
    char *name;
    void *func;
} MethodCalls;

static MethodCalls methods[]={
    {"CreateRequest",&host_create_request},
    {"AddHeader",&host_add_header},
    {"AddServerVariable",&host_add_server_variable},
    {"HeadersSent",&host_headers_sent},
    {"AddBodyPart",&host_add_body_part},
    {NULL,NULL}
};

void
register_transport (MonoReflectionType *transport_type)
{
    MonoType *type = mono_reflection_type_get_type(transport_type);
    MonoClass *klass = mono_class_from_mono_type(type);
    MonoImage* image=mono_class_get_image(klass);
    char *fullname = g_strdup_printf("%s.%s",mono_class_get_namespace(klass), mono_class_get_name(klass));
    char *method_name;
    MonoMethodDesc* mdesc;
    MonoMethod* method;

    int i=0;
    while (methods[i].name)
    {
        method_name = g_strdup_printf("%s:%s",fullname,methods[i].name);
        mdesc = mono_method_desc_new (method_name, TRUE);
        method = mono_method_desc_search_in_image(mdesc, image);
        if (!method) {
            ERROR_OUT("Can't find method %s",method_name);
        }
        *(uintptr_t *)methods[i].func = (uintptr_t)mono_method_get_unmanaged_thunk (method);
        mono_method_desc_free(mdesc);
        g_free(method_name);
        i++;
    }

    g_free (fullname);
}

void bridge_register_icall ()
{
   mono_add_internal_call ("Mono.WebServer.HyperFastCgi.Transport.NativeTransport::RegisterHost",register_host);
   mono_add_internal_call ("Mono.WebServer.HyperFastCgi.Transport.NativeTransport::RegisterTransport",register_transport);
   mono_add_internal_call ("Mono.WebServer.HyperFastCgi.Transport.NativeTransport::SendOutput",bridge_send_output);
   mono_add_internal_call ("Mono.WebServer.HyperFastCgi.Transport.NativeTransport::EndRequest",bridge_end_request);

}


