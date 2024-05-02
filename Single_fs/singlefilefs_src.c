#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>
#include <linux/time.h>
#include <linux/buffer_head.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>

#include "singlefilefs.h"


/*
Questi comandi sono parte di un processo per creare un filesystem utilizzando un'immagine di un file e montarlo nel sistema. 
Ecco cosa fanno:

1. **create-fs:**
   - `dd bs=4096 count=100 if=/dev/zero of=image`: Questo comando crea un file chiamato "image" con 
   dimensioni di 4096 byte * 100 = 400 KB. Il contenuto del file è composto da zeri, poiché viene copiato 
   dal dispositivo `/dev/zero`.
   - `./singlefilemakefs image`: Questo comando utilizza il programma `singlefilemakefs` (presumibilmente 
   un programma personalizzato per creare un filesystem su un singolo file) per formattare 
   l'immagine appena creata come un filesystem.
   - `mkdir mount`: Questo comando crea una directory chiamata "mount" nel percorso corrente.

2. **mount-fs:**
   - `sudo mount -o loop -t singlefilefs image ./mount/`: Questo comando monta l'immagine 
   del filesystem creato precedentemente (immagine "image") nella directory appena creata "mount". 
   L'opzione `-o loop` consente di montare un file come dispositivo loop. L'opzione `-t singlefilefs` specifica il
    tipo di filesystem da montare (presumibilmente "singlefilefs" è un tipo di filesystem personalizzato).*/


static struct super_operations singlefilefs_super_ops = {
};


static struct dentry_operations singlefilefs_dentry_ops = {
};


int singlefilefs_fill_super(struct super_block *sb, void *data, int silent) {   

    struct inode* root_inode;
    struct buffer_head *bh;
    struct onefilefs_sb_info *sb_disk;
    struct timespec64 curr_time;
    uint64_t magic;

    //Unique identifier of the filesystem
    sb->s_magic = MAGIC;

    bh = sb_bread(sb, SB_BLOCK_NUMBER);
    if(!sb){
	return -EIO;
    }
    sb_disk = (struct onefilefs_sb_info *)bh->b_data;
    magic = sb_disk->magic;
    brelse(bh); // Rilascio del buffer_head dopo l'uso

    //check on the expected magic number
    if(magic != sb->s_magic){
	return -EBADF;
    }

    sb->s_fs_info = NULL; //FS specific data (the magic number) already reported into the generic superblock
    sb->s_op = &singlefilefs_super_ops;//set our own operations


    root_inode = iget_locked(sb, SINGLEFILEFS_ROOT_INODE_NUMBER);//get a root inode from cache
    if (!root_inode){
        return -ENOMEM;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,12,0)
inode_init_owner(sb->s_user_ns, root_inode, NULL, S_IFDIR);//set the root user as owner of the FS root
#else
inode_init_owner(root_inode, NULL, S_IFDIR);//set the root user as owner of the FS root
#endif

    
    root_inode->i_sb = sb;
    root_inode->i_op = &onefilefs_inode_ops;//set our inode operations
    root_inode->i_fop = &onefilefs_dir_operations;//set our file operations
    //update access permission
    root_inode->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH;

    //baseline alignment of the FS timestamp to the current time
    ktime_get_real_ts64(&curr_time);
    root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = curr_time;

    // no inode from device is needed - the root of our file system is an in memory object
    root_inode->i_private = NULL;

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root)
        return -ENOMEM;

    sb->s_root->d_op = &singlefilefs_dentry_ops;//set our dentry operations

    //unlock the inode to make it usable
    unlock_new_inode(root_inode);

    return 0;
}

static void singlefilefs_kill_superblock(struct super_block *s) {
    kill_block_super(s);
    printk(KERN_INFO "%s: singlefilefs unmount succesful.\n",MOD_NAME);
    return;
}

//called on file system mounting 
struct dentry *singlefilefs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {

    struct dentry *ret;

    ret = mount_bdev(fs_type, flags, dev_name, data, singlefilefs_fill_super);

    if (unlikely(IS_ERR(ret)))
        printk("%s: error mounting onefilefs",MOD_NAME);
    else
        printk("%s: singlefilefs is succesfully mounted on from device %s\n",MOD_NAME,dev_name);

    return ret;
}

//file system structure
static struct file_system_type onefilefs_type = {
	.owner = THIS_MODULE,
        .name           = "singlefilefs",
        .mount          = singlefilefs_mount,
        .kill_sb        = singlefilefs_kill_superblock,
};

static int singlefilefs_init(void) {

    int ret;

    //register filesystem
    ret = register_filesystem(&onefilefs_type);
    if (likely(ret == 0))
        printk("%s: sucessfully registered singlefilefs\n",MOD_NAME);
    else
        printk("%s: failed to register singlefilefs - error %d", MOD_NAME,ret);

    return ret;
}

static void singlefilefs_exit(void) {

    int ret;
    
    //filp_close(destFile, NULL); qui chiudo il file unico
    
    //unregister filesystem
    ret = unregister_filesystem(&onefilefs_type);

    if (likely(ret == 0))
        printk("%s: sucessfully unregistered file system driver\n",MOD_NAME);
    else
        printk("%s: failed to unregister singlefilefs driver - error %d", MOD_NAME, ret);
}

module_init(singlefilefs_init);
module_exit(singlefilefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roberto Fardella <roberto.fard@gmail.com>");
MODULE_DESCRIPTION("SINGLE-FILE-FS");
