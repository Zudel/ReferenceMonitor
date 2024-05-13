#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>
#include <linux/time.h>
#include <linux/buffer_head.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "singlefilefs.h"

#define MODNAME "single-fs"

//this iterate function just returns 3 entries: . and .. and then the name of the unique file of the file system
static int onefilefs_iterate(struct file *file, struct dir_context* ctx) {

    	//printk("%s: we are inside readdir with ctx->pos set to %lld", MODNAME, ctx->pos);
	
	if(ctx->pos >= (2 + 1)) return 0;//we cannot return more than . and .. and the unique file entry

	if (ctx->pos == 0){
    		//printk("%s: we are inside readdir with ctx->pos set to %lld", MODNAME, ctx->pos);
		if(!dir_emit(ctx,".", FILENAME_MAXLEN, SINGLEFILEFS_ROOT_INODE_NUMBER, DT_UNKNOWN)){
			return 0;
		}
		else{
			ctx->pos++;
		}
	
	}

	if (ctx->pos == 1){
    		//printk("%s: we are inside readdir with ctx->pos set to %lld", MODNAME, ctx->pos);
		//here the inode number does not care
		if(!dir_emit(ctx,"..", FILENAME_MAXLEN, 1, DT_UNKNOWN)){
			return 0;
		}
		else{
			ctx->pos++;
		}
	
	}
	if (ctx->pos == 2){
    		//printk("%s: we are inside readdir with ctx->pos set to %lld", MODNAME, ctx->pos);
		if(!dir_emit(ctx, UNIQUE_FILE_NAME, FILENAME_MAXLEN, SINGLEFILEFS_FILE_INODE_NUMBER, DT_UNKNOWN)){
			return 0;
		}
		else{
			ctx->pos++;
		}
	
	}

	return 0;

}

const struct file_operations onefilefs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate = onefilefs_iterate,
};
/*
Il file `fs/readdir.c` è un componente del kernel Linux e contiene il codice per l'implementazione della funzione `readdir()` nel sistema di file. Questa funzione è utilizzata per leggere le voci (file e directory) all'interno di una directory specifica. Quando un programma richiama `readdir()`, il sistema operativo esegue il codice in `fs/readdir.c` per ottenere le informazioni sui file e sulle directory presenti nella directory specificata.

All'interno di `fs/readdir.c`, troverai il codice che gestisce le operazioni di lettura delle voci di directory, l'inoltro di queste informazioni al chiamante e la gestione degli errori e delle eccezioni che potrebbero verificarsi durante l'operazione di lettura della directory.

In sintesi, `fs/readdir.c` serve a implementare il comportamento della funzione `readdir()` nel kernel Linux, consentendo ai programmi utente di ottenere informazioni sui file e sulle directory presenti in una directory specifica nel sistema di file.*/