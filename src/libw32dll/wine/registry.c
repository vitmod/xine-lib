#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

#include "winbase.h"
#include "winreg.h"
#include "winnt.h"
#include "winerror.h"
#include "debugtools.h"

#include <ext.h>
#include <registry.h>

//#undef TRACE
//#define TRACE printf

struct reg_value
{
	int type;
	char* name;
	int len;
	char* value;
};

// ...can be set before init_registry() call
char* regpathname = 0;

static int reg_size=0;
static struct reg_value* regs = 0;

struct reg_handle_s;
typedef struct reg_handle_s
{
	int handle;
	char* name;
	struct reg_handle_s* next;
	struct reg_handle_s* prev;
} reg_handle_t;
		
static reg_handle_t* head=0;

#define DIR -25

static void create_registry(void);
static void open_registry(void);
static void save_registry(void);
static void init_registry(void);




static void create_registry(void){
    if(regs)
    {
	printf("Logic error: create_registry() called with existing registry\n");
	save_registry();
	return;
    }	
    regs=(struct reg_value*)malloc(3*sizeof(struct reg_value));
    regs[0].type=regs[1].type=DIR;
    regs[0].name=(char*)malloc(5);
    strcpy(regs[0].name, "HKLM");
    regs[1].name=(char*)malloc(5);
    strcpy(regs[1].name, "HKCU");
    regs[0].value=regs[1].value=NULL;
    regs[0].len=regs[1].len=0;
    reg_size=2;
    save_registry();
}
static void open_registry(void)
{
	int fd;
	int i;
	unsigned int len;
	if(regs)
	{
		printf("Multiple open_registry(>\n");
		return;
	}
	fd = open(regpathname, O_RDONLY);
	if (fd == -1)
	{
	    printf("Creating new registry\n");
	    create_registry();
	    return;
	}    
	read(fd, &reg_size, 4);
	regs=(struct reg_value*)malloc(reg_size*sizeof(struct reg_value));
	for(i=0; i<reg_size; i++)
	{
		read(fd,&regs[i].type,4);
		read(fd,&len,4);
		regs[i].name=(char*)malloc(len+1);
		if(regs[i].name==0)
		{
			reg_size=i+1;
			goto error;
		}
		read(fd, regs[i].name, len);
		regs[i].name[len]=0;
		read(fd,&regs[i].len,4);
		regs[i].value=(char*)malloc(regs[i].len+1);
		if(regs[i].value==0)
		{
			free(regs[i].name);
			reg_size=i+1;
			goto error;
		}
		read(fd, regs[i].value, regs[i].len);
		regs[i].value[regs[i].len]=0;
	}
error:
	close(fd);
	return;
}

static void save_registry(void)
{
	int fd, i;
	if (!regs)
		init_registry();
	fd = open(regpathname, O_WRONLY | O_CREAT, 00666);
	if (fd == -1)
	{
	    printf("Failed to open registry file '%s' for writing.\n",
		   regpathname);
	    return;
	}
	write(fd, &reg_size, 4);
	for(i=0; i<reg_size; i++)
	{
	        unsigned len=strlen(regs[i].name);
		write(fd, &regs[i].type, 4);
		write(fd, &len, 4);
		write(fd, regs[i].name, len);
		write(fd, &regs[i].len, 4);
		write(fd, regs[i].value, regs[i].len);
	}
	close(fd);
}
static reg_handle_t* find_handle_by_name(const char* name)
{
	reg_handle_t* t;
	for(t=head; t; t=t->prev)
	{
		if(!strcmp(t->name, name))
		{
			return t;
		}
	}
	return 0;
}
static struct reg_value* find_value_by_name(const char* name)
{
	int i;
	for(i=0; i<reg_size; i++)
		if(!strcmp(regs[i].name, name))
			return regs+i;
	return 0;
}
static reg_handle_t* find_handle(int handle)
{
	reg_handle_t* t;
	for(t=head; t; t=t->prev)
	{
		if(t->handle==handle)
		{
			return t;
		}
	}
	return 0;
}	
static int generate_handle()
{
	static int zz=249;
	zz++;
	while((zz==HKEY_LOCAL_MACHINE) || (zz==HKEY_CURRENT_USER))
		zz++;
	return zz;
}

static reg_handle_t* insert_handle(long handle, const char* name)
{
	reg_handle_t* t;
	t=(reg_handle_t*)malloc(sizeof(reg_handle_t));
	if(head==0)
	{
		t->prev=0;
	}
	else
	{
		head->next=t;
		t->prev=head;
	}
	t->next=0;
	t->name=(char*)malloc(strlen(name)+1);
	strcpy(t->name, name);
	t->handle=handle;
	head=t;
	return t;
}
static char* build_keyname(long key, const char* subkey)
{
	char* full_name;
	reg_handle_t* t;
 	if((t=find_handle(key))==0)
	{
		TRACE("Invalid key\n");
		return NULL;
	}
	if(subkey==NULL)
		subkey="<default>";
	full_name=(char*)malloc(strlen(t->name)+strlen(subkey)+10);
	strcpy(full_name, t->name);
	strcat(full_name, "\\");
	strcat(full_name, subkey);
	return full_name;
}
static struct reg_value* insert_reg_value(int handle, const char* name, int type, const void* value, int len)
{
	reg_handle_t* t;
	struct reg_value* v;
	char* fullname;
	if((fullname=build_keyname(handle, name))==NULL)
	{
		TRACE("Invalid handle\n");
		return NULL;
	}

	if((v=find_value_by_name(fullname))==0)
	//creating new value in registry
	{
		if(regs==0)
		    create_registry();
		regs=(struct reg_value*)realloc(regs, sizeof(struct reg_value)*(reg_size+1));
		v=regs+reg_size;
		reg_size++;
	}
	else
	//replacing old one
	{
		free(v->value);
		free(v->name);
	}
	v->type=type;
	v->len=len;
	v->value=(char*)malloc(len);
	memcpy(v->value, value, len);
	v->name=(char*)malloc(strlen(fullname)+1);
	strcpy(v->name, fullname);
	save_registry();
	return v;
}

static void init_registry(void)
{
	struct passwd* pwent;
	TRACE("Initializing registry\n");
	pwent = getpwuid(geteuid());
	// can't be free-ed - it's static and probably thread
	// unsafe structure which is stored in glibc

	if (regpathname == 0)
	{
	    regpathname = (char*)malloc(strlen(pwent->pw_dir)+20);
	    strcpy(regpathname, pwent->pw_dir);
	    strcat(regpathname, "/.registry");
	}

	open_registry();
	insert_handle(HKEY_LOCAL_MACHINE, "HKLM");
	insert_handle(HKEY_CURRENT_USER, "HKCU");
}

static reg_handle_t* find_handle_2(long key, const char* subkey)
{
	char* full_name;
	reg_handle_t* t;
 	if((t=find_handle(key))==0)
	{
		TRACE("Invalid key\n");
		return (reg_handle_t*)-1;
	}
	if(subkey==NULL)
		return t;
	full_name=(char*)malloc(strlen(t->name)+strlen(subkey)+10);
	strcpy(full_name, t->name);
	strcat(full_name, "\\");
	strcat(full_name, subkey);
	t=find_handle_by_name(full_name);
	free(full_name);
	return t;
}

long RegOpenKeyExA(long key, const char* subkey, long reserved, long access, int* newkey)
{
    char* full_name;
    reg_handle_t* t;
    struct reg_value* v;
    TRACE("Opening key %s\n", subkey);
    
    if(!regs)
        init_registry()
;	
/*	t=find_handle_2(key, subkey);
	
	if(t==0)
		return -1;

	if(t==(reg_handle_t*)-1)
		return -1;
*/
    full_name=build_keyname(key, subkey);
    if(!full_name)
        return -1;
    TRACE("Opening key Fullname %s\n", full_name);
    v=find_value_by_name(full_name);    

    t=insert_handle(generate_handle(), full_name);
    *newkey=t->handle;
    free(full_name);
    
    return 0;
}    
long RegCloseKey(long key)
{
        reg_handle_t *handle;
    if(key==HKEY_LOCAL_MACHINE)
	return 0;
    if(key==HKEY_CURRENT_USER)
	return 0;
    handle=find_handle(key);
    if(handle==0)
	return 0;
    if(handle->prev)
	handle->prev->next=handle->next;
    if(handle->next)
	handle->next->prev=handle->prev;
    if(handle->name)
	free(handle->name);
    if(handle==head)
	head=head->prev;
    free(handle);
    return 1;
}         
long RegQueryValueExA(long key, const char* value, int* reserved, int* type, int* data, int* count)
{
	struct reg_value* t;
	char* c;
	TRACE("Querying value %s\n", value);
	if(!regs)
	    init_registry()
;	
    	c=build_keyname(key, value);
	if(c==NULL)
		return 1;
	if((t=find_value_by_name(c))==0)
	{
		free(c);
		return 2;
	}
	free(c);
	if(type)
		*type=t->type;
	if(data)
	{
		memcpy(data, t->value, (t->len<*count)?t->len:*count);
		TRACE("returning %d bytes: %d\n", t->len, *(int*)data);
	}	
		if(*count<t->len)
		{
			*count=t->len;
			return ERROR_MORE_DATA;
    }
    else
    {
        *count=t->len;
	}
    return 0;
}  
long RegCreateKeyExA(long key, const char* name, long reserved,
		     void* classs, long options, long security,
		     void* sec_attr, int* newkey, int* status)
{
	reg_handle_t* t;
	char* fullname;
	struct reg_value* v;
//        TRACE("Creating/Opening key %s\n", name);
        TRACE("Creating/Opening key %s\n", name);
	if(!regs)
	    init_registry()
;	
	fullname=build_keyname(key, name);
	if(fullname==NULL)
		return 1;
	v=find_value_by_name(fullname);
	if(v==0)
	{
		int qw=45708;
		v=insert_reg_value(key, name, DIR, &qw, 4);
		if (status) *status=REG_CREATED_NEW_KEY;
//		return 0;
	}
	else
	{
            // this is a hack as I don't know how RegEnumValueA works
	    if (strstr(fullname, "zlib") || strstr(fullname, "mszh"))
                return 1;
	    if (status) *status=REG_OPENED_EXISTING_KEY;
	}

	t=insert_handle(generate_handle(), fullname);
	*newkey=t->handle;
	free(fullname);
	return 0;
}

long RegEnumValueA(HKEY hkey, DWORD index, LPSTR value, LPDWORD val_count,
		   LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD count)
{
    // have no idea how this should work
    //printf("Reg Enum 0x%x %d  %p %d   data: %p %d  %d >%s<\n", hkey, index, value, *val_count, data, *count, reg_size, data);
    {
	reg_handle_t* t = find_handle(hkey);
	if (t)
	{
	    struct reg_value* v=find_value_by_name(t->name);
            *count = v->len;
	    memcpy(data, v->value, *count);
            *val_count = v->len;
	    memcpy(value, v->value, *val_count);
            if (type)
		*type = v->type;
	    //printf("Found handle  %s\n", v->name);
            return 0;
	}
    }

    return -1;
}

long RegSetValueExA(long key, const char* name, long v1, long v2, const void* data, long size)
{
    struct reg_value* t;
    char* c;
    TRACE("Request to set value %s\n", name);
    if(!regs)
        init_registry()
;	
    c=build_keyname(key, name);
    if(c==NULL)
	return 1;
    insert_reg_value(key, name, v2, data, size);
    free(c);
    return 0;
}        
