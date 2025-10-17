#include "sender.h"
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define FTOK_PATH "/tmp"
#define SHM_TEXT_SIZE 1024 //給每則訊息在shared memory 中保留 1024 bytes 的空間
#define EXIT_MARKER "__GETOUT__"

#define SEM_SENDER_NAME   "/sem_sender_lab"
#define SEM_RECEIVER_NAME "/sem_receiver_lab"


/* Helper: open or create named semaphore; set *is_creator = 1 if created */
//用來修復無法先開receiver再開sender
// sem_t* ：POSIX 命名 semaphore 的 handle
static sem_t *open_or_create_sem(const char *name, unsigned int initial, int *is_creator) {
    sem_t *s;
    *is_creator = 0; //預設不是建立者
    errno = 0;
    s = sem_open(name, O_CREAT | O_EXCL, 0600, initial); //O_CREAT | O_EXCL：這組旗標表示「如果物件不存在就建立並回傳；如果已存在則失敗並回傳 SEM_FAILED 並把 errno 設為 EEXIST」
    if (s != SEM_FAILED) {
        *is_creator = 1; //代表成功建立新的命名semaphore（其他同時嘗試的 process 在這一步會失敗並得 errno==EEXIST）
        return s;
    }
    if (errno != EEXIST) { //代表失敗原因是其他錯誤
        perror("sem_open O_CREAT|O_EXCL");
        return SEM_FAILED;
    }
    /* already exists, open existing (do not change initial value) */
    s = sem_open(name, 0); //打開計有的semaphore
    if (s == SEM_FAILED) {
        perror("sem_open existing");
        return SEM_FAILED;
    }
    return s;
}


/* send wrapper: assumes caller handled synchronization (no internal busy-wait) */
void send(message_t message, mailbox_t* mailbox_ptr){

    /*  TODO: 
        1. Use flag to determine the communication method
        2. According to the communication method, send the message
    */
    
    if (mailbox_ptr->flag == MSG_PASSING) { // #define MSG_PASSING = 1
        int msqid = mailbox_ptr->storage.msqid; //message queue id
        if (msqid < 0) { perror("send: invalid msqid"); return; }
        size_t send_len = strnlen(message.msgText, sizeof(message.msgText)-1) + 1;//計算要傳給 msgsnd() 的 byte 長度
        if (msgsnd(msqid, &message, send_len, 0) == -1) { //msgsnd: from System V
            perror("msgsnd");
        }
    } else if (mailbox_ptr->flag == SHARED_MEM) {
        char *base = mailbox_ptr->storage.shm_addr; //取出 shared memory 的 mapped 位址
        if (!base) { fprintf(stderr, "send: invalid shm addr\n"); return; }
        int *status = (int*)base; // 0 empty, 1 full, 2 exit (aux) status非必要存在
        char *buf = base + sizeof(int); //一個字元緩衝區 buf（用來放訊息字串）
        // buf 是指向 shared memory 節中的資料區，sender 寫入它、receiver 從它讀出
        /* caller guarantees buffer is free (via semaphores) */
        strncpy(buf, message.msgText, SHM_TEXT_SIZE-1);
        // 將 message.msgText 複製到 shared memory 的 buf，使用 strncpy 並在最後強制 '\0'，以避免沒有終止字元情況造成讀取錯誤或 buffer over-read
        buf[SHM_TEXT_SIZE-1] = '\0';
        *status = (strcmp(message.msgText, EXIT_MARKER) == 0) ? 2 : 1;
    } else {
        fprintf(stderr, "send: unknown mailbox flag %d\n", mailbox_ptr->flag);
    }
}

int main(int argc, char **argv){

    /*  TODO: 
        1) Call send(message, &mailbox) according to the flow in slide 4
        2) Measure the total sending time
        3) Get the mechanism and the input file from command line arguments
            • e.g. ./sender 1 input.txt
                    (1 for Message Passing, 2 for Shared Memory)
        4) Get the messages to be sent from the input file
        5) Print information on the console according to the output format
        6) If the message form the input file is EOF, send an exit message to the receiver.c
        7) Print the total sending time and terminate the sender.c
    */
    
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <mechanism(1=msgq,2=shm)> <input.txt>\n", argv[0]);
        return 1;
    }
    
    int mech = atoi(argv[1]); //mechanism
    char *inputfile = argv[2];

    mailbox_t mailbox;
    memset(&mailbox, 0, sizeof(mailbox)); //declare
    mailbox.flag = mech;

    /* Setup IPC resource depending on mechanism */
    if (mech == MSG_PASSING) {
        key_t key = ftok(FTOK_PATH, 0x66); //System V IPC 的 key，給 msgget() 使用
        // 0x66只是隨便選的一個ID，給message queue用
        // 用法上是方便多個程式或執行個體產生同一個 key 以共享 IPC 資源
        // ftok是一個簡單的方法讓不同程式產生相同的key
        if (key == -1) { perror("ftok msg"); return 1; }
        int msqid = msgget(key, IPC_CREAT | 0666); //呼叫 msgget() 取得或建立 System V message queue，回傳一個 msqid
        //0666 是 UNIX 權限（owner/group/others 可讀寫） rw-rw-rw-
        // msqid 是 kernel 持有的 message-queue 物件的 id
        // 發訊息： msgsnd(msqid, &msgbuf, msgsz, 0);
        // 收訊息： msgrcv(msqid, &msgbuf, bufsize, msgtyp, 0);
        // 刪 queue： msgctl(msqid, IPC_RMID, NULL);
        if (msqid == -1) { perror("msgget"); return 1; }
        mailbox.storage.msqid = msqid; //存進mailbox
        printf("Message Passing\n");
    } else if (mech == SHARED_MEM) {
        key_t key = ftok(FTOK_PATH, 0x55);
        if (key == -1) { perror("ftok shm"); return 1; }
        size_t shm_size = sizeof(int) + SHM_TEXT_SIZE;
        int shmid = shmget(key, shm_size, IPC_CREAT | 0666); //呼叫 shmget() 取得或建立一個 System V shared memory segment，並回傳 shmid(shared memory id)
        if (shmid == -1) { perror("shmget"); return 1; }
        void *shmaddr = shmat(shmid, NULL, 0); //shmat() 把 shmid 對應的 shared memory segment attach到目前 process 的位址空間，回傳address
        if (shmaddr == (void*)-1) { perror("shmat"); return 1; }
        /* initialize status to 0 (empty) */
        int *status = (int*)shmaddr;
        *status = 0; //0代表buffer為空 1代表有資料 2代表exit
        mailbox.storage.shm_addr = (char*)shmaddr; //把 shmaddr（cast 為 char*）存在 mailbox 的 union 裡面，讓 send() / receive() 可以使用相同的位址來寫/讀 shared memory。
        printf("Shared Memory\n");
    } else {
        return 0;
    }

    /* 打開或建立兩個 POSIX named semaphore */
    int sender_created = 0, receiver_created = 0;
    sem_t *sem_sender = open_or_create_sem(SEM_SENDER_NAME, 1, &sender_created); //創建semaphore 初始值設1
    if (sem_sender == SEM_FAILED) return 1;
    sem_t *sem_receiver = open_or_create_sem(SEM_RECEIVER_NAME, 0, &receiver_created);
    if (sem_receiver == SEM_FAILED) { //檢測是否已經開啟
        sem_close(sem_sender);
        return 1;
    }

    /*
    // Setup semaphores: unlink first to reset, then open 
    sem_unlink(SEM_SENDER_NAME);
    sem_unlink(SEM_RECEIVER_NAME);
    
    sem_t *sem_sender = sem_open(SEM_SENDER_NAME, O_CREAT, 0666, 1); // sender allowed first
    sem_t *sem_receiver = sem_open(SEM_RECEIVER_NAME, O_CREAT, 0666, 0); //receiver blocked
    if (sem_sender == SEM_FAILED || sem_receiver == SEM_FAILED) {
        perror("sem_open");
        return 1;
    }
    */

    FILE *fp = fopen(inputfile, "r");
    
    struct timespec start, end; //time count
    double total_comm_time = 0.0;

    char line[SHM_TEXT_SIZE + 128];
    message_t msg;
    msg.mType = 1;

    while (fgets(line, sizeof(line), fp)) { //send
        // Prepare message (not timed)
        size_t ln = strlen(line);
        if (ln > 0 && line[ln-1] == '\n') line[ln-1] = '\0'; //去除尾端換行
        
        strncpy(msg.msgText, line, sizeof(msg.msgText)-1); //字串複製到msg.msgText
        msg.msgText[sizeof(msg.msgText)-1] = '\0';

        sem_wait(sem_sender); //在每次要送一筆訊息之前呼叫，目的是取得發送權
        /*
                        若 sem_sender 的計數 > 0：減 1 並立刻返回；sender 立刻可以送訊息（不被阻塞）。
                        若 sem_sender 的計數 == 0：sender 在此呼叫處阻塞，直到 receiver 做 
        */

        // count time
        clock_gettime(CLOCK_MONOTONIC, &start);
        send(msg, &mailbox);
        clock_gettime(CLOCK_MONOTONIC, &end); //只算send前後
        total_comm_time += (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9; //calculate

        printf("Sending message:\t%s\n", msg.msgText);

        // Unblock receiver
        sem_post(sem_receiver); // 每則訊息送完後 喚醒 receiver 去接收
    }

    /* EOF: send exit marker and print End... */
    sem_wait(sem_sender); //先等到 sender 可以取得 semaphore（確保與 receiver 的同步），準備送 exit 訊息
    strncpy(msg.msgText, EXIT_MARKER, sizeof(msg.msgText)-1);
    msg.msgText[sizeof(msg.msgText)-1] = '\0';

    clock_gettime(CLOCK_MONOTONIC, &start);
    send(msg, &mailbox); /* measured: writing exit marker is communication */
    clock_gettime(CLOCK_MONOTONIC, &end);
    total_comm_time += (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9; //發送exit訊息也須列入計算

    printf("End of input file! exit!\n");
    sem_post(sem_receiver); // 喚醒 receiver 去接收exit

    printf("Total time taken in sending msg: %.6f s\n", total_comm_time);

    // cleanup
    sem_close(sem_sender);
    sem_close(sem_receiver);
    
    //如果使用SHM，要解除連結
    if (mech == SHARED_MEM) {
        shmdt((void*)mailbox.storage.shm_addr);
    }
    fclose(fp);
    return 0;
}

/*
Sender 每次：

1. sem_wait(sem_sender)（取得發送權，否則阻塞）
2. send(...)（做實際通訊；對 message-queue 內部也可能阻塞）
3. sem_post(sem_receiver)（喚醒 receiver 去接收）

Receiver 每次：
1. sem_wait(sem_receiver)（等待 sender 喚醒，否則阻塞）
2. receive(...)（做實際接收；對 message-queue 也可能阻塞）
3. sem_post(sem_sender)（喚醒 sender 發下一筆）

wait : -1
post : +1
*/

