#ifndef FSS_API_H
#define FSS_API_H

#include <posixver.h>

#include <stddef.h>
#include <time.h>

extern char fss_verbose;

/**
 * Store a file on disk (in secondary memory) in the existent specified directory
 *
 * Return 0 on success, -1 on error (set errno)
 */
int store_file(const char* abs_pathname, const char* content, const size_t size, const char* directory);

/**
 * Sleep for an interval measured in milliseconds
 */
void sleep_for(const long msec);

/**
 * Viene aperta una connessione AF_UNIX al socket file sockname.
 * Se il server non accetta immediatamente la richiesta di connessione,
 * la connessione da parte del client viene ripetuta dopo 'msec' millisecondi
 * e fino allo scadere del tempo assoluto 'abstime' specificato come terzo argomento.
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int openConnection(const char* sockname, int msec, const struct timespec abstime);

/**
 * Chiude la connessione AF_UNIX associata al socket file sockname.
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int closeConnection(const char* sockname);

/**
 * Richiesta di apertura o di creazione di un file.
 * La semantica della openFile dipende dai flags passati come secondo argomento
 * che possono essere O_CREATE ed O_LOCK.
 * Se viene passato il flag O_CREATE ed il file esiste gia' memorizzato nel server,
 * oppure il file non esiste ed il flag O_CREATE non e' stato specificato, viene ritornato un errore.
 * In caso di successo, il file viene sempre aperto in lettura e scrittura,
 * ed in particolare le scritture possono avvenire solo in append.
 * Se viene passato il flag O_LOCK (eventualmente in OR con O_CREATE)
 * il file viene aperto e/o creato in modalita locked,
 * che vuol dire che l'unico che puo leggere o scrivere il file 'pathname' e' il processo che lo ha aperto.
 * Il flag O_LOCK puo' essere esplicitamente resettato utilizzando la chiamata unlockFile, descritta di seguito.
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int openFile(const char* pathname, int flags);

/**
 * Legge tutto il contenuto del file dal server (se esiste)
 * ritornando un puntatore ad un'area allocata sullo heap nel parametro 'buf',
 * mentre 'size' conterra' la dimensione del buffer dati (ossia la dimensione in bytes del file letto).
 * In caso di errore, 'buf' e 'size' non sono validi.
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int readFile(const char* pathname, void** buf, size_t* size);

/**
 * Richiede al server la lettura di 'N' files qualsiasi da memorizzare nella directory 'dirname' lato client.
 * Se il server ha meno di 'N' file disponibili, li invia tutti.
 * Se N<=0 la richiesta al server e' quella di leggere tutti i file memorizzati al suo interno.
 * Ritorna un valore maggiore o uguale a 0 in caso di successo (cioe' ritorna il n. di file effettivamente letti),
 * -1 in caso di fallimento, errno viene settato opportunamente.
 */
int readNFiles(int N, const char* dirname);

/**
 * Scrive tutto il file puntato da pathname nel file server.
 * Ritorna successo solo se la precedente operazione, terminata con successo, e' stata
 * openFile(pathname, O_CREATE| O_LOCK).
 * Se 'dirname' e' diverso da NULL, il file eventualmente spedito dal server
 * perche' espulso dalla cache per fare posto ai nuovi dati di 'pathname' dovra' essere scritto in 'dirname';
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int writeFile(const char* pathname, const char* dirname);

/**
 * Richiesta di scrivere in append al file 'pathname' i 'size' bytes contenuti nel buffer 'buf'.
 * L'operazione di append nel file e' garantita essere atomica dal file server.
 * Se 'dirname' e' diverso da NULL, il file eventualmente spedito dal server
 * perche' espulso dalla cache per far posto ai nuovi dati di 'pathname' dovra' essere scritto in 'dirname';
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int appendToFile(const char* pathname, void* buf, size_t size, const char* dirname);

/**
 * In caso di successo setta il flag O_LOCK al file.
 * Se il file era stato aperto/creato con il flag O_LOCK e la richiesta proviene dallo stesso processo,
 * oppure se il file non ha il flag O_LOCK settato, l'operazione termina immediatamente con successo,
 * altrimenti l'operazione non viene completata fino a quando il flag O_LOCK non viene resettato
 * dal detentore della lock.
 * L'ordine di acquisizione della lock sul file non e' specificato.
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int lockFile(const char* pathname);

/**
 * Resetta il flag O_LOCK sul file 'pathname'.
 * L'operazione ha successo solo se l'owner della lock e' il processo che ha richiesto l'operazione,
 * altrimenti l'operazione termina con errore.
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int unlockFile(const char* pathname);

/**
 * Richiesta di chiusura del file puntato da 'pathname'.
 * Eventuali operazioni sul file dopo la closeFile falliscono.
 * Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
 */
int closeFile(const char* pathname);

/**
 * Rimuove il file cancellandolo dal file storage server.
 * L'operazione fallisce se il file non e' in stato locked,
 * o e' in stato locked da parte di un processo client diverso da chi effettua la removeFile.
 */
int removeFile(const char* pathname);

#endif
