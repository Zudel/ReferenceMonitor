#include <linux/syscalls.h>
#include <linux/version.h>
#include <linux/module.h>
#include<linux/mount.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/unistd.h> // Include per geteuid()
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/namei.h> //kern_path()
#include <linux/path.h>
#include "referenceMonitor.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roberto Fardella <roberto.fard@gmail.com>");
MODULE_DESCRIPTION("Linux Security Module (LSM) for file protection");

/* The kernel functions we want to hooks: */
const char* security_file_open_hook_name = "security_file_open";
const char* security_inode_setattr_hook_name = "security_inode_setattr";
const char* security_inode_create_hook_name = "security_inode_create";
const char* security_inode_link_hook_name = "security_inode_link";
const char* security_inode_unlink_hook_name = "security_inode_unlink";
const char* security_inode_symlink_hook_name = "security_inode_symlink";
const char* security_inode_rmdir_hook_name = "security_inode_rmdir";
const char* security_inode_mknod_hook_name = "security_inode_mknod";
const char* security_inode_mkdir_hook_name = "security_inode_mkdir" ;
const char* security_inode_rename_hook_name = "security_inode_rename";
static ref_mon *rm;
unsigned long *nisyscall;
unsigned long cr0;

void deferred_logger_handler(struct work_struct* data);
static int the_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int security_file_open_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_create_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_link_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_unlink_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_symlink_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_mkdir_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_rmdir_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_mknod_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_rename_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);
static int inode_setattr_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs);

declare_kretprobe(security_inode_create_probe, inode_create_pre_hook, the_hook,sizeof(struct log_info));
declare_kretprobe(security_file_open_probe, security_file_open_pre_hook, the_hook,sizeof(struct log_info));
declare_kretprobe(security_inode_link_probe, inode_link_pre_hook, the_hook,sizeof(struct log_info));
declare_kretprobe(security_inode_unlink_probe, inode_unlink_pre_hook, the_hook,sizeof(struct log_info));
declare_kretprobe(security_inode_symlink_probe, inode_symlink_pre_hook, the_hook, sizeof(struct log_info));
declare_kretprobe(security_inode_rmdir_probe, inode_rmdir_pre_hook, the_hook, sizeof(struct log_info));
declare_kretprobe(security_inode_mkdir_probe, inode_mkdir_pre_hook, the_hook,sizeof(struct log_info));
declare_kretprobe(security_inode_mknod_probe, inode_mknod_pre_hook, the_hook, sizeof(struct log_info));
declare_kretprobe(security_inode_rename_probe, inode_rename_pre_hook, the_hook,sizeof(struct log_info));
declare_kretprobe(security_inode_setattr_probe, inode_setattr_pre_hook, the_hook,sizeof(struct log_info));

/*sys_switch_state: cambiamento dello stato del reference monitor*/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
__SYSCALL_DEFINEx(2,_switch_state, enum rm_state, state, char* , pw){
#else
asmlinkage int sys_switch_state(enum state, char* pw){
#endif
    int ret, i, offset = 0;
    const struct cred *cred = current_cred();
    char* pw_buffer;
    unsigned char* hash_digest;

    if (!uid_eq(cred->euid, GLOBAL_ROOT_UID)){ // Verifica se l'UID effettivo è root
        printk(KERN_INFO "Only the actual root UID can change the status.\n");
        return -EPERM; 
    }

    pw_buffer = safe_copy_from_user(pw);
    if(!pw_buffer){
        printk("%s: error in safe_copy_from_user\n", MODNAME);
        return -ENOMEM;
    }

    hash_digest = password_hash(pw_buffer);
    if(!hash_digest){
        printk("%s: computation passwprd hash failed\n", MODNAME);
        return -ENOMEM;
    }
    kfree(pw_buffer); 
    printk("%s: hash rm %s, hash calcolata %s", MODNAME, rm->pw_hash, hash_digest);

    if( strcmp(rm->pw_hash, hash_digest) != 0 ){   
        printk("%s: mismatching of the password\n", MODNAME);
        return -EINVAL; 
        }
    
     
    if(rm->state == state) {
        printk("%s: the entered state is already the current one\n", MODNAME);
        return -EINVAL;
    }
    kfree(hash_digest);
    switch (state)
    {
      case ON:
        printk("%s:The inserted state is ON\n", MODNAME);
        rm->state = ON;
        break;

    case OFF:
        printk("%s:The inserted state is OFF\n", MODNAME);
        rm->state = OFF;
        break;

    case REC_ON:
        printk("%s:The inserted state is REC_ON\n", MODNAME);
        rm->state = REC_ON;
        break;

    case REC_OFF:    
        printk("%s:The inserted state is REC_OFF\n", MODNAME);
        rm->state = REC_OFF;
        break;

    default:
        printk("%s:The inserted state is not valid\n", MODNAME);
        return -EINVAL;
    }
    return rm->state;
}

/*sys_manage_link: */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
__SYSCALL_DEFINEx(3,_manage_link, char*, buffer_path, char*, pw, int, op){
#else
asmlinkage int sys_manage_link(char* buffer_path,char* pw, int op){
#endif
    int ret;
    const struct cred *cred = current_cred();
    struct inode *inode;
    node * node_ptr;
    int error;
    struct list_head *ptr;
    struct path struct_path;
    struct list_head * new_node_lh;
    unsigned char* hash_digest;
    char* pathname;
    char* pw_buffer;
    
    if (!uid_eq(cred->euid, GLOBAL_ROOT_UID)){ 
        printk("%s: Only EUID 0 (root) can perform the insert/delete path activity\n", MODNAME);
        return -EPERM; 
    }

    if(rm->state == OFF || rm->state == ON){
        printk("%s: Switch to REC-ON or REC-OFF state in order to perform the insert/delete path activity\n", MODNAME);
        return -EINVAL;    
    }

    if(op == 2) goto print; //we skip the passage of parameters from user space to kernel space

    pw_buffer = safe_copy_from_user(pw);
    if(!pw_buffer){
        printk("%s: error in safe_copy_from_user\n", MODNAME);
        return -ENOMEM;
    }

    hash_digest = password_hash(pw_buffer);
    if(!hash_digest){
        printk("%s: computation passwprd hash failed\n", MODNAME);
        return -ENOMEM;
    }
    kfree(pw_buffer); 

    if( strcmp(rm->pw_hash, hash_digest) != 0 ){   
        printk("%s: mismatching of the password\n", MODNAME);
        return -EINVAL; 
        }

    pathname = safe_copy_from_user(buffer_path);
    if(!pathname){
        printk("%s: error in safe_copy_from_user\n", MODNAME);
        return -ENOMEM;
    }
    error=kern_path(pathname,LOOKUP_FOLLOW, &struct_path);
    if(error){
        kfree(pathname);
        printk("%s:kern_path failed, the file or directory doesn't exists \n", MODNAME);
        return -ENOMEM;
    }

print:
    spin_lock(&rm->lock);

    if(op == 0){ //adding path
            
            new_node_lh = kmalloc(sizeof(struct list_head), GFP_KERNEL);
            if (new_node_lh == NULL) {
                spin_unlock(&rm->lock);
                printk("%s:allocation of new node into the list failed \n",MODNAME);
                return -ENOMEM;
            }
            
            node_ptr = list_entry(new_node_lh, node, elem); 
            node_ptr->path = kstrdup(pathname,GFP_KERNEL);
            if(!node_ptr->path){
                spin_unlock(&rm->lock);
                printk("%s: kstrdup failed\n", MODNAME);
                return -ENOMEM;
            }
            
            inode =  struct_path.dentry->d_inode; //retrieve inode from kern_path
            
            /*check if inode is already present*/
            if(list_empty(&rm->blk_head_node.elem)) goto ok; //if balcklist is empty, no check if a path is already present
            
            node *node_ptr_aux;
            list_for_each(ptr, &rm->blk_head_node.elem) {
                node_ptr_aux = list_entry(ptr, node, elem);
                
                if(node_ptr_aux->inode_cod == inode->i_ino){
                    printk("%s: the path %s is already present!\n",MODNAME, node_ptr->path);
                    kfree(new_node_lh);
                    kfree(node_ptr->path);
                    spin_unlock(&rm->lock);
                    return -EINVAL;
                }
            }
ok:
            node_ptr->inode_cod = inode->i_ino;
            node_ptr->inode_blk = inode;
            node_ptr->dentry_blk = struct_path.dentry;
            kfree(pathname);
            list_add_tail(new_node_lh, &rm->blk_head_node.elem);  // Adding the new node to the blacklist
            spin_unlock(&rm->lock);
    }
    else if(op == 1){ //removing path
        if(list_empty(&rm->blk_head_node.elem)){
            kfree(pathname);
            spin_unlock(&rm->lock);
            printk("%s: the blacklist is empty\n", MODNAME);
            return -EFAULT;
        }
        list_for_each(ptr, &rm->blk_head_node.elem) {
            node_ptr = list_entry(ptr, node, elem);
            if(strcmp(node_ptr->path , pathname) == 0){ 
                list_del(ptr);
                kfree(pathname);
                spin_unlock(&rm->lock);
                printk("%s: path removed correctly \n", MODNAME);
                return 0;
            }
        }
        kfree(pathname);
        spin_unlock(&rm->lock);
        printk("%s: path to remove not found \n", MODNAME);
        return -EINVAL;
    }
    else{ //list path , the first parameter pathname of the syscall sys_manage_link, can be NULL
          ////if the value of op is 2, print the paths of the blacklist (they need to be seen via dmesg)
        if(list_empty(&rm->blk_head_node.elem)){
            spin_unlock(&rm->lock);
            printk("%s: the blacklist is empty\n", MODNAME);
            return -EFAULT;
        }

        printk("%s: blacklist:\n", MODNAME);
        list_for_each(ptr, &rm->blk_head_node.elem) {
            node_ptr = list_entry(ptr, node, elem); 
            printk("%s: (address element %p, inode->i_ino %lu, path %s, inode %p, dentry %p, prev = %p, next = %p)\n",MODNAME, ptr, node_ptr->inode_cod, node_ptr->path, node_ptr->inode_blk, node_ptr->dentry_blk, ptr->prev, ptr->next);               
        }
        spin_unlock(&rm->lock);
    }
    
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
static unsigned long sys_switch_state = (unsigned long) __x64_sys_switch_state;	     
static unsigned long sys_manage_link = (unsigned long) __x64_sys_manage_link;   
#endif

unsigned long systemcall_table=0x0;
module_param(systemcall_table,ulong,0660);
char* password;
module_param(password, charp, 0444); // 0444 imposta i permessi di sola lettura (ro)
int free_entries[15];
module_param_array(free_entries,int,NULL,0660);

static inline void write_cr0_forced(unsigned long val){
    unsigned long __force_order;

    /* __asm__ __volatile__( */
    asm volatile(
        "mov %0, %%cr0"
        : "+r"(val), "+m"(__force_order));
}

static inline void protect_memory(void){
    write_cr0_forced(cr0);
}

static inline void unprotect_memory(void){
    write_cr0_forced(cr0 & ~X86_CR0_WP);
}

/**
 * int (*inode_permission)(struct inode *inode, int mask);
 * Check permission before accessing an inode (Write access must be blocked here ). 
 * This hook is called by the existing Linux permission function, so a security module 
 * can use it to provide additional checking for existing Linux permission checks. 
 * Notice that this hook is called when a file is opened (as well as many other operations), whereas the 
 * file_security_ops permission hook is called when the actual read/write operations are performed. 
 * inode contains the inode structure to check. 
 * mask contains the permission mask. Return 0 if permission is granted.
*/

 int security_file_open_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct file* file;
    file = (struct file*)regs->di;
    struct log_info* log_info;
    node* node_ptr_h;
    fmode_t mode = file->f_mode;
    struct list_head* ptr_h;
    
    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
    list_for_each(ptr_h, &rm->blk_head_node.elem) {
        node_ptr_h = (node*)list_entry(ptr_h, node, elem);
        if(!node_ptr_h) goto leave;
        if((node_ptr_h->inode_cod  == file->f_inode->i_ino)  &&  ((mode & FMODE_WRITE) || (mode & FMODE_PWRITE))){  
                printk("%s: write file denied\n", MODNAME);
                log_info = (struct log_info*) ri->data;
                log_info->pathname = node_ptr_h->path;
                spin_unlock(&rm->lock);
                return 0;
        }
    }
leave:
    spin_unlock(&rm->lock);
    return 1; 
 }

/* int security_inode_create(struct inode *dir, struct dentry *dentry, umode_t mode)
Parameters
struct inode *dir - the parent directory
struct dentry *dentry - the file being created
umode_t mode - requested file mode
Description
Check permission to create a regular file. 
dir contains inode structure of the parent of the new file. 
dentry contains the dentry structure for the file to be created.
 mode contains the file mode of the file to be created.
Returns 0 if permission is granted.
*/

int inode_create_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct inode* parent_inode = (struct inode*)regs->di;
    struct dentry* parent_dentry;
    node* node_ptr_h;
    struct list_head* ptr_h;
    struct log_info* log_info;
    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
    parent_dentry = d_find_alias(parent_inode);
    list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            if(!node_ptr_h) goto leave;
            if((parent_inode->i_ino == node_ptr_h->inode_cod)|| (is_subdir(parent_dentry,node_ptr_h->dentry_blk))){
                        printk("%s: vfs_create denied\n ", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        spin_unlock(&rm->lock);
                        return 0;
            }
    }
leave:
    spin_unlock(&rm->lock);
    return 1; 
}

/*int security_inode_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry);
   called in vfs_link - create a new link
 * @old_dentry:	object to be linked
 * @dir:	new parent
 * @new_dentry:	where to create the new link
 * @description: Check permission before creating a new hard link to a file. 
 * old_dentry contains the dentry structure for an existing link to the file. 
 * dir contains the inode structure of the parent directory of the new link. 
 * new_dentry contains the dentry structure for the new link. 
 * */

int inode_link_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct dentry* old_dentry; //dentry structure for an existing link to the file
    struct inode* parent_inode; //parent inode dir of the new link
    parent_inode = (struct inode* )regs->si;
     old_dentry = (struct dentry* )regs->di;
    //struct dentry* new_dentry = regs->dx; // dentry structure for the new link
    //struct inode* inode = old_dentry->d_inode;
    struct dentry* parent_dentry; 
    node* node_ptr_h;
    struct list_head* ptr_h;
    struct log_info* log_info;

    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
    parent_dentry = d_find_alias(parent_inode);
    list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            if(!node_ptr_h) goto leave;
            if(node_ptr_h->inode_cod == old_dentry->d_inode->i_ino || (is_subdir(parent_dentry,node_ptr_h->dentry_blk))){
                        printk("%s: vfs_link denied\n ", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        spin_unlock(&rm->lock);
                        return 0;
            }
        }
leave:
    spin_unlock(&rm->lock);
    return 1;
}
/*int security_inode_unlink(struct inode *dir, struct dentry *dentry) 
 * called in vfs_unlink - unlink a filesystem object
 * @dir:	parent directory
 * @dentry:	victim
 * 
 * @description: Check the permission to remove a hard link to a file. 
 * dir contains the inode structure of parent directory of the file. 
 * dentry contains the dentry structure for file to be unlinked.
 *  Return 0 if permission is granted.
 * */
int inode_unlink_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
        struct inode* parent_inode; //parent inode dir 
        struct log_info* log_info;
        struct dentry* dentry; //dentry for file to be unlinked
        parent_inode = (struct inode* )regs->di;
        dentry = (struct dentry*) regs->si;
        node* node_ptr_h;
        struct list_head* ptr_h;
        struct dentry* parent_dentry;

        spin_lock(&rm->lock);
        if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
        parent_dentry = d_find_alias(parent_inode); //dentry structure for an existing link to the file
        list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            
            if((dentry->d_inode->i_ino == node_ptr_h->inode_cod) || (is_subdir(parent_dentry,node_ptr_h->dentry_blk))){
                        printk("%s: vfs_unlink denied\n ", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        spin_unlock(&rm->lock);
                        return 0;
            }
        }

leave:
    spin_unlock(&rm->lock);
    return 1;
}
/* int security_inode_symlink(struct inode *dir, struct dentry *dentry, const char *old_name)

 * called in vfs_symlink - create symlink
 * @dir:	inode of @dentry
 * @dentry:	pointer to dentry of the base directory
 * @oldname:	name of the file to link to
 *
 * @description: Check the permission to create a symbolic link to a file. 
 * dir contains the inode structure of parent directory of the symbolic link. 
 * dentry contains the dentry structure of the symbolic link. 
 * old_name contains the pathname of file. Return 0 if permission is granted.
*/
int inode_symlink_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct inode* parent_inode;
    parent_inode = (struct inode*)regs->di;
    //struct dentry* dentry = (struct dentry*)regs->si;
    struct log_info* log_info;
    //char* old_name = (char*)regs->dx;
    node* node_ptr_h;
    struct list_head* ptr_h;
    
    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
    
    list_for_each(ptr_h, &rm->blk_head_node.elem) {
        node_ptr_h = (node*)list_entry(ptr_h, node, elem);
        if(!node_ptr_h) goto leave;
        if(parent_inode->i_ino == (get_parent_inode(node_ptr_h->inode_blk))->i_ino || is_subdir(d_find_alias(parent_inode), node_ptr_h->dentry_blk) ){
                    printk("%s: vfs_symlink denied\n ", MODNAME);
                    log_info = (struct log_info*) ri->data;
                    log_info->pathname = node_ptr_h->path;
                    spin_unlock(&rm->lock);
                    return 0;
        }
    }
leave:
    spin_unlock(&rm->lock);
    return 1;
}
/* int security_inode_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) */
/**
 * called in vfs_mkdir - create directory
 * @dir:	inode of @dentry
 * @dentry:	pointer to dentry of the base directory
 *
 * @description:Check permissions to create a new directory in the existing directory associated with inode structure dir. 
 * dir contains the inode structure of parent of the directory to be created. 
 * dentry contains the dentry structure of new directory. 
 * mode contains the mode of new directory. Return 0 if permission is granted.
 * */
int inode_mkdir_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct inode* parent_inode;
    parent_inode = (struct inode*)regs->di;
    //struct dentry* dentry = (struct dentry*)regs->si;
    struct log_info* log_info;
    node* node_ptr_h;
    struct list_head* ptr_h;
    struct dentry* parent_dentry;

    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
    parent_dentry = d_find_alias(parent_inode);
    list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            if(!node_ptr_h) goto leave;
            if((parent_dentry->d_inode->i_ino == (get_parent_inode(node_ptr_h->inode_blk))->i_ino) || (is_subdir(parent_dentry,node_ptr_h->dentry_blk))){
                        printk("%s: vfs_mkdir denied\n ", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        spin_unlock(&rm->lock);
                        return 0;
            }
    }
leave:
    spin_unlock(&rm->lock);
    return 1;
}
/*int security_inode_rmdir(struct inode *dir, struct dentry *dentry) 
 * called in vfs_rmdir - remove directory
 * @dir:	inode of @dentry
 * @dentry:	pointer to dentry of the base directory
 *
 * @description:Check the permission to remove a directory. 
 * dir contains the inode structure of parent of the directory to be removed. 
 * dentry contains the dentry structure of directory to be removed. 
 * Return 0 if permission is granted.
*/
int inode_rmdir_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    //struct inode* parent_inode = (struct inode*)regs->di;
    struct dentry* dentry;
    dentry = (struct dentry*)regs->si;
    struct log_info* log_info;
    node* node_ptr_h;
    struct list_head* ptr_h;
    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;

    list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            if(!node_ptr_h) goto leave;
            if(dentry->d_inode->i_ino == node_ptr_h->inode_cod || (is_subdir(dentry,node_ptr_h->dentry_blk))){
                        printk("%s: vfs_rmdir denied\n", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        spin_unlock(&rm->lock);
                        return 0;
            }
    }
leave:
    spin_unlock(&rm->lock);
    return 1;
}

/* int security_inode_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)*/
/**
 * called into vfs_mknod - create device node or file
 * @dir:	inode of @dentry
 * @dentry:	pointer to dentry of the base directory
 * @mode:	mode of the new device node or file
 * @dev:	device number of device to create
 * @description: Check permissions when creating a special file (or a socket or a fifo file created via the mknod system call). 
 * Note that if mknod operation is being done for a regular file, then the create hook will be called and not this hook. 
 * dir contains the inode structure of parent of the new file. dentry contains the dentry structure of the new file. 
 * mode contains the mode of the new file. 
 * dev contains the device number. Return 0 if permission is granted.
 */
int inode_mknod_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct inode* inode = (struct inode*)regs->di;
    struct log_info *log_info;
    node* node_ptr_h;
    struct list_head* ptr_h;
    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;

    list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            if(!node_ptr_h) goto leave;
            if(inode->i_ino == node_ptr_h->inode_cod || (is_subdir(d_find_alias(inode),node_ptr_h->dentry_blk))){
                        printk("%s: vfs_rmdir denied\n ", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        spin_unlock(&rm->lock);
                        return 0;
            }
    }

leave:
    spin_unlock(&rm->lock);
    return 1;
}

/*int security_inode_rename(struct inode *old_dir, struct dentry *old_dentry,struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) 
 * called in vfs_rename - rename a filesystem object
 * @description: Check for permission to rename a file or directory. 
 * old_dir contains the inode structure for parent of the old link. 
 * old_dentry contains the dentry structure of the old link. 
 * new_dir contains the inode structure for parent of the new link. 
 * new_dentry contains the dentry structure of the new link. Return 0 if permission is granted.
 * */
int inode_rename_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct  dentry* old_dentry;
    struct log_info *log_info;
    //struct dentry* new_dentry = (struct dentry*)regs->cx;
    struct inode* old_inode;
    //struct inode* new_inode = new_dentry->d_inode;
    node* node_ptr_h;
    struct list_head* ptr_h;
    old_dentry = (struct dentry*)regs->si;
    old_inode = old_dentry->d_inode;

    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
    list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            if(!node_ptr_h) goto leave;
            if(old_inode->i_ino == node_ptr_h->inode_cod){
                        printk("%s: vfs_rename denied\n ", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        spin_unlock(&rm->lock);
                        return 0;
                
            }
    }
leave:
    spin_unlock(&rm->lock);
    return 1;
}

/**
 * security_inode_setattr() - Check if setting file attributes is allowed
 * @dentry: file
 * @attr: new attributes
 *
 * Check permission before setting file attributes.  Note that the kernel call
 * to notify_change is performed from several locations, whenever file
 * attributes change (such as when a file is truncated, chown/chmod operations,
 * transferring disk quotas, etc).
 *
 * Return: Returns 0 if permission is granted.
 */
int inode_setattr_pre_hook(struct kretprobe_instance  *ri, struct pt_regs *regs){
    struct dentry* dentry = (struct dentry*)regs->di;
    node * node_ptr_h;
    struct log_info* log_info;
    struct list_head *ptr_h;
    unsigned long i_ino = dentry->d_inode->i_ino;

    spin_lock(&rm->lock);
    if(list_empty(&rm->blk_head_node.elem) || ((rm->state == REC_OFF || rm->state == OFF ))) goto leave;
    list_for_each(ptr_h, &rm->blk_head_node.elem) {
            node_ptr_h = (node*)list_entry(ptr_h, node, elem);
            if(!node_ptr_h) goto leave;
            if(i_ino == node_ptr_h->inode_cod){
                        spin_unlock(&rm->lock);
                        printk("%s: chmod denied\n ", MODNAME);
                        log_info = (struct log_info*) ri->data;
                        log_info->pathname = node_ptr_h->path;
                        return 0;
            }
    }

leave:
    spin_unlock(&rm->lock);
    return 1;
}


/* The_hook function is the exit handler shared among all the kretprobes.
It blocks any attempt to write access and performs deferred work to write 
various log information to a file.
*/
int the_hook(struct kretprobe_instance *ri, struct pt_regs *regs){
    regs->ax = -EACCES; 
    struct log_info *log_info;
    log_info = (struct log_info*) ri->data;
    logging_information(rm, log_info);
    return 0;
}

/*
The following deferred_logger_handler function will be executed as deferred work every time the exit handler 'the_hook' is invoked.
Will write the following information:
the process TGID
the thread ID
the user-id
the effective user-id
the program path-name that is currently attempting the open
a cryptographic hash of the program file content
*/

void deferred_logger_handler(struct work_struct* data){ 
    packed_work *pkd_w;
    int i, ret,offset = 0;
    char line[4096];

    pkd_w = container_of(data, packed_work , work);
    
    printk("%s", pkd_w->log_info.pathname);
    if(!pkd_w->log_info.pathname){
        printk("%s: pathname not valid\n", MODNAME);
        return;
    }
    
    pkd_w->log_info.file_content_hash = file_content_fingerprint(pkd_w->log_info.pathname); //dovrei usar eun hash non crittografico
    if(!(pkd_w->log_info.file_content_hash)){
        printk("%s: hash not computed\n", MODNAME);
        return;
    }
    sprintf(line, "pathname: %s, file content hash: %s, tgid: %d, tid: %d, effective uid: %d, real uid: %d\n", pkd_w->log_info.pathname,pkd_w->log_info.file_content_hash,pkd_w->log_info.tgid, pkd_w->log_info.tid, pkd_w->log_info.effect_uid, pkd_w->log_info.real_uid);
 
    ret = kernel_write(rm->log_file, line, strlen(line), &rm->log_file->f_pos);
    if(IS_ERR(ret != strlen(line))){
        printk(KERN_ERR "%s: Failed to write into the unique file: %d\n", MODNAME, ret);
        return;
    }

    printk("%s: ret %d write data to file: %s\n",MODNAME, ret, line);
    return;
}

int init_module(void) {
    unsigned long ** sys_call_table;
    char* digest_crypto_hash;

    /* initializing struct ref_mon rm */
    rm =  kmalloc(sizeof(ref_mon), GFP_KERNEL); //alloc memory in kernel space

    if(unlikely(!rm)){
        printk(KERN_ERR "%s: failure in init module\n", MODNAME);
        return -ENOMEM;
    }
    if(!password || strlen(password) == 0){
        printk("%s: password is unset\n", MODNAME);
        return -EINVAL;
    }
    digest_crypto_hash = password_hash(password);//the password has been passed as a parameter of the module
    
    if(!digest_crypto_hash){
        printk("%s: failed to install the password in the reference monitor\n", MODNAME);
        return 0;
    }

    rm->pw_hash = kstrdup(digest_crypto_hash, GFP_KERNEL);
    
    kfree(digest_crypto_hash);
    
    rm->state = OFF;// init state of reference monitor
    
    INIT_LIST_HEAD(&rm->blk_head_node.elem); 
    
    rm->queue_work = alloc_workqueue("REFERENCE_MONITOR_WORKQUEUE", WQ_MEM_RECLAIM, 1); // create an own workqueue 
    if(unlikely(!rm->queue_work)) {
        printk(KERN_ERR "%s: creation workqueue failed\n", MODNAME);
        return -1;
    }

    rm->log_file = filp_open("./Single_fs/mount/the-file", O_RDWR, 0);
	if (IS_ERR(rm->log_file)) {
        printk(KERN_ERR "%s: Failed to open log-file\n", MODNAME);
        return PTR_ERR(rm->log_file);
    }

    printk("%s: the unique file opened correctly\n", MODNAME);

    /* registering kretprobes*/

    security_file_open_probe.kp.symbol_name = security_file_open_hook_name;
    security_inode_create_probe.kp.symbol_name = security_inode_create_hook_name;
    security_inode_link_probe.kp.symbol_name = security_inode_link_hook_name;
    security_inode_unlink_probe.kp.symbol_name = security_inode_unlink_hook_name;
    security_inode_symlink_probe.kp.symbol_name = security_inode_symlink_hook_name;
    security_inode_rmdir_probe.kp.symbol_name = security_inode_rmdir_hook_name;
    security_inode_mkdir_probe.kp.symbol_name = security_inode_mkdir_hook_name;
    security_inode_mknod_probe.kp.symbol_name = security_inode_mknod_hook_name;
    security_inode_rename_probe.kp.symbol_name = security_inode_rename_hook_name;
    security_inode_setattr_probe.kp.symbol_name = security_inode_setattr_hook_name;
    
    set_kretprobe(&security_file_open_probe);
    set_kretprobe(&security_inode_create_probe);
    set_kretprobe(&security_inode_link_probe);
    set_kretprobe(&security_inode_unlink_probe);
    set_kretprobe(&security_inode_symlink_probe);
    set_kretprobe(&security_inode_rmdir_probe);
    set_kretprobe(&security_inode_mkdir_probe);
    set_kretprobe(&security_inode_mknod_probe);
    set_kretprobe(&security_inode_rename_probe);
    set_kretprobe(&security_inode_setattr_probe);

    /*installing system calls*/
    if(systemcall_table!=0){
        cr0 = read_cr0();
        unprotect_memory();
        sys_call_table = (void*) systemcall_table; 
        nisyscall = sys_call_table[free_entries[0]]; //mi serve poi nel cleanup
        sys_call_table[free_entries[0]] = (unsigned long*)sys_switch_state;
        sys_call_table[free_entries[1]] = (unsigned long*)sys_manage_link;
        protect_memory();
    }else{
        printk("%s: system call table not avalaible\n", MODNAME);
        return -1;
    }
        printk("%s: module correctly mounted\n", MODNAME);    
        return 0;

}

void cleanup_module(void) {
    unsigned long ** sys_call_table;
   
    /*restore system call table*/
    cr0 = read_cr0();
    unprotect_memory();
    sys_call_table = (void*) systemcall_table; 
    sys_call_table[free_entries[0]] = nisyscall;
    sys_call_table[free_entries[1]] = nisyscall;
    protect_memory();   
     
    /* unregistering kretprobes*/
    unregister_kretprobe(&security_inode_create_probe);
    unregister_kretprobe(&security_file_open_probe);
    unregister_kretprobe(&security_inode_link_probe);
    unregister_kretprobe(&security_inode_unlink_probe);
    unregister_kretprobe(&security_inode_symlink_probe);
    unregister_kretprobe(&security_inode_rmdir_probe);
    unregister_kretprobe(&security_inode_mkdir_probe);
    unregister_kretprobe(&security_inode_mknod_probe);
    unregister_kretprobe(&security_inode_rename_probe);
    unregister_kretprobe(&security_inode_setattr_probe);
    
    /*releasing resources*/
    destroy_workqueue(rm->queue_work); 
    kfree(rm->pw_hash);
    if (likely(rm->log_file)) filp_close(rm->log_file, NULL);
    kfree(rm);
    
    printk("%s: shutting down\n",MODNAME);
}
