#include <linux/syscalls.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/unistd.h> // Include per geteuid()
#include <linux/cred.h>
#include <linux/kprobes.h>
#include <linux/errno.h>
#include <linux/namei.h>
#include <linux/path.h>
#include "referenceMonitor.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roberto Fardella <roberto.fard@gmail.com>");
MODULE_DESCRIPTION("my first module");

#define MODNAME "reference_monitor"

#define PERMS 0644
#define SHA256_DIGEST_SIZE 16
#define MAX_PW_SIZE 64
#define KEY_DESC "my_password_key"
#define FILE_PATH "./pw.txt" // Definisci il percorso del file
#define target_func "filp_open" //you should modify this depending on the kernel version

static ref_mon *rm;
static struct kretprobe retprobe;
static struct task_struct *thread;
static spinlock_t lock;
static spinlock_t lock_sys_write;

node * node_ptr ;
struct list_head* new_node;
struct list_head *ptr;
unsigned long * nisyscall;
unsigned long cr0;


/*sys_switch_state: cambiamento dello stato del reference monitor*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
__SYSCALL_DEFINEx(3,_switch_state, enum rm_state, state, char* , pw, size_t, size){
#else
asmlinkage int sys_switch_state(enum state, char*  pw, size_t size){
#endif
    int ret;
    const struct cred *cred = current_cred();
        unsigned char * hash_digest = kmalloc(SHA256_DIGEST_SIZE*2 + 1, GFP_KERNEL); // GFP_KERNEL specifica che l'allocazione avviene in contesto di kernel
    printk(KERN_INFO "%s: system call switch state invocata correttamente con parametri state %d, pw %s, taglia %d", MODNAME, state, pw, (int)size);
   
   
    if (!uid_eq(cred->euid, GLOBAL_ROOT_UID)){ // Verifica se l'UID effettivo è root
        printk(KERN_INFO "Solo l'UID effettivo root può cambiare lo stato.\n");
        return -EPERM; // Restituisci errore di permesso negato
    }

	
    char pw_buffer[MAX_PW_SIZE];
    void *addr;
    if(size >= (MAX_PW_SIZE -1)) return -EINVAL;

    addr = (void*)get_zeroed_page(GFP_KERNEL);
    
    if (addr == NULL) return -ENOMEM;

    ret = copy_from_user((char*)addr, pw, (int) size);
    
    memcpy(pw_buffer,(char*)addr,size-ret);
    
    pw_buffer[size - ret] = '\0';
    free_pages((unsigned long)addr,0);

    if (calculate_hash(pw_buffer, hash_digest)  < 0) {  // Verifica della password (da implementare)
        printk(KERN_INFO "%s: Password non valida.\n", MODNAME);
        return -EINVAL; // Restituisci errore di input non valido
    }
        int i, offset = 0;
        char buffer[SHA256_DIGEST_SIZE * 2 + 1]; // Il buffer per contenere l'output esadecimale dei byte più il terminatore di stringa
    
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%02x", hash_digest[i]); // Formattare due caratteri esadecimali per ogni byte
    }

    buffer[offset] = '\0'; // Aggiungi il terminatore di stringa
    printk(KERN_INFO "\nHash della password formattato :  %s", buffer);

    size_t hash_len = sizeof(buffer); // Lunghezza dell'hash della password

    write_to_file(buffer, hash_len); // Chiamata alla funzione per scrivere

    printk(KERN_INFO "%s: Password hash key created\n", MODNAME);
 
    if(rm->state == state) {
        printk("lo stato inserito e' gia' quello corrente");
        return -1;
    }
    
    switch (state)
    {
      case ON:
        printk(KERN_INFO  "lo stato inserito e' ON");
        rm->state = ON;
        if( disable_kretprobe(&retprobe) != 0 )
            printk("disabling retproble failed \n");
        break;
    case OFF:
        printk(KERN_INFO  "lo stato inserito e' OFF");
        rm->state = OFF;
        if( disable_kretprobe(&retprobe) != 0 )
            printk("disabling retproble failed \n");
        break;
    case REC_ON:
        printk(KERN_INFO  "lo stato inserito e' REC_ON");
        rm->state = REC_ON;
        if( enable_kretprobe(&retprobe) != 0 )
            printk("abiliting  retproble failed \n");
        break;
    case REC_OFF:    
        printk(KERN_INFO "lo stato inserito e' REC_OFF");
        rm->state = REC_OFF;
        if( enable_kretprobe(&retprobe) != 0 )
            printk("abiliting  retproble failed \n");
        break;
    default:
        printk(KERN_INFO "lo stato inserito non e' valido");
        break;
    }

    return rm->state;
}

/*sys_add_or_remove_link: aggiunta o rimozione del path all'insieme da protezioni aperture in modalità scrittura*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
__SYSCALL_DEFINEx(2,_manage_link, char*, path, int, op){
#else
asmlinkage int sys_manage_link(char* pathname,int op){
#endif

    int ret;
    const struct cred *cred = current_cred();
    struct file *file;
    int  nbytes;
    loff_t offset;
    struct inode *inode;
    struct path path;
    
    printk(KERN_INFO "%s: system call sys_manage_link invocata correttamente con parametri %s %d ", MODNAME, path, op);
    if(rm->state == OFF || rm->state == ON){
        printk("%s: passare allo stato di REC-ON oppure REC-OFF per poter eseguire l'attivita' di inserimento/eliminazione del path", MODNAME);
        return -EPERM;    
    }
    
    if (!uid_eq(cred->euid, GLOBAL_ROOT_UID)){ // Verifica se l'UID effettivo è root
        printk(KERN_INFO "Solo l'UID effettivo root può eseguire l'attivita' di inserimento/eliminazione del path.\n");
        return -EPERM; // Restituisci errore di permesso negato
    }
    
    if(op == 0){ //path da aggiungere alla lista
            spin_lock(&lock);
            new_node = kmalloc(sizeof(struct list_head),GFP_KERNEL);
            if (new_node == NULL) {
                printk("allocation of new node into the list failed/n");
                return -ENOMEM;
            }
            node_ptr = list_entry(new_node, node, list); 
            node_ptr->path = pathname;
            kern_path(pathname, LOOKUP_FOLLOW , &path );
            inode =  path.dentry->d_inode;
            node_ptr->inode_cod = inode->i_ino;
            spin_lock(&lock);
            list_add_tail(new_node, &rm->paths.list);  // Aggiunta del nuovo nodo alla lista
            spin_unlock(&lock);
        
    }
    else if(op == 1){ //path da rimuovere dalla lista
        if(list_empty(&rm->paths.list)){
            printk("%s: the set of paths to protect is empty \n", MODNAME);
            return -EFAULT;
        }

        list_for_each(ptr, &rm->paths.list) {
            node_ptr = list_entry(ptr, node, list); //utilizza internamente container_of()
            if(strcmp(node_ptr->path , pathname) == 0){ //qui andrebbe il path dato dall'utente
                spin_lock(&lock);
                list_del(ptr);
                spin_unlock(&lock);
                printk("%s: path removed correctly \n", MODNAME);
                return 0;
            }           
        }
        printk("%s: path to remove not found \n", MODNAME);
        return -EINVAL;

    }
    else{ //visualizzo la lista dei path da proteggere da
        if(list_empty(&rm->paths.list)){
            printk("%s: the set of paths to protect is empty \n", MODNAME);
            return -EFAULT;
        }

        printk("%s: lista dei path che non sono accessibili in scrittura", MODNAME);

        list_for_each(ptr, &rm->paths.list) {
            node_ptr = list_entry(ptr, node, list); //utilizza internamente container_of()
                       
            printk(KERN_ALERT "(list %p, value %lu, path %s, prev = %p, next = %p) \n",ptr, filp->f_dentry->d_inode->i_ino, ptr->path, ptr->prev, ptr->next); 
                        
        }
    }

return 0;

}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
static unsigned long sys_switch_state = (unsigned long) __x64_sys_switch_state;	
static unsigned long sys_manage_link = (unsigned long) __x64_sys_manage_link;	
#endif


unsigned long systemcall_table=0x0;
module_param(systemcall_table,ulong,0660);
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

static int sys_open_wrapper(struct kprobe *ri, struct pt_regs *regs){
        //where to look at when searching system call parmeters
     struct file * filp;
     unsigned long inode_id;
    if(rm->state == ON || rm->state == OFF) goto end;

    filp = (struct file*) regs_return_value(regs);
    
    if ( filp == NULL) goto end;//check if the filp_open operation actually returned file pointer
    
    else{
        inode_id = filp->f_dentry->d_inode->i_ino; 
        list_for_each(ptr, &rm->paths.list) {
            node_ptr = list_entry(ptr, node, list);
            if(node_ptr->inode_cod == inode_id){ //qui va o il codice inode o il path, dove lo prendo??
                printk("%s: the file associated to %ld inode has been open ", MODNAME, inode_id );
                write_to_file("file test in utils aperto", "./test.txt");
                goto end;
            }
    }
        
    }
end:
    return 0;
}

int init_module(void) {
    unsigned long ** sys_call_table;
    int ret = 0;
     int  nbytes;
     char * buffer;
    loff_t offset = 0;
     size_t count1;
    
    rm =  kmalloc(sizeof(ref_mon), GFP_KERNEL); //alloc memory and setup reference monitor struct
    rm->state = REC_ON;// init state of reference monitor
    INIT_LIST_HEAD(&rm->paths.list); //inizializzo la struttura list_head in rm

    retprobe.kp.symbol_name = target_func;
	retprobe.handler = (kretprobe_handler_t) sys_open_wrapper;
	retprobe.entry_handler = (kretprobe_handler_t)NULL;
	retprobe.maxactive = -1; //lets' go for the default number of active kretprobes manageable by the kernel

	ret = register_kretprobe(&retprobe); //register the kretprobe for the filp_open to intercept the file opened
	if (ret < 0) {
		printk("%s: hook init failed , returned %d\n", MODNAME, ret);
		return ret;
	}

    //if(!try_module_get(THIS_MODULE)) return -1;
    if(systemcall_table!=0){
        cr0 = read_cr0();
        unprotect_memory();
        sys_call_table = (void*) systemcall_table; 
        nisyscall = sys_call_table[free_entries[0]]; //mi serve poi nel cleanup
        sys_call_table[free_entries[0]] = (unsigned long*)sys_switch_state;
        sys_call_table[free_entries[1]] = (unsigned long*)sys_manage_link;
        protect_memory();
    }else{
        printk("%s: system call table non trovata\n", MODNAME);
        return -1;
    }
    
        printk("%s: module correctly mounted\n", MODNAME);    
        return ret;

}

void cleanup_module(void) {
    int ret;
    unsigned long ** sys_call_table;
    //module_put(THIS_MODULE);

    /*restore system call table*/
    cr0 = read_cr0();
    unprotect_memory();
    sys_call_table = (void*) systemcall_table; 
    sys_call_table[free_entries[0]] = nisyscall;
    sys_call_table[free_entries[1]] = nisyscall;
    protect_memory();   

    unregister_kretprobe(&retprobe);
    
    kfree(rm);
    printk("%s: shutting down\n",MODNAME);

}
  

  