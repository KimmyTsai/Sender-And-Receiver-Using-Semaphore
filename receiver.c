#include "receiver.h"
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

static sem_t *open_or_create_sem(const char *name, unsigned int initial, int *is_creator) {
    sem_t *s;
    *is_creator = 0;
    errno = 0;
    s = sem_open(name, O_CREAT | O_EXCL, 0600, initial);
    if (s != SEM_FAILED) {
        *is_creator = 1;
        return s;
    }
    if (errno != EEXIST) {
        perror("sem_open O_CREAT|O_EXCL");
        return SEM_FAILED;
    }
    s = sem_open(name, 0);
    if (s == SEM_FAILED) {
        perror("sem_open existing");
        return SEM_FAILED;
    }
    return s;
}


/* receive wrapper: assumes caller handled synchronization (no internal busy-wait) */
void receive(message_t* message_ptr, mailbox_t* mailbox_ptr){

    /*  TODO: 
        1. Use flag to determine the communication method
        2. According to the communication method, receive the message
    */
    
    // (1) 判斷模式
    
    if (mailbox_ptr->flag == MSG_PASSING) {
        int msqid = mailbox_ptr->storage.msqid;
        if (msqid < 0) { perror("receive: invalid msqid"); return; }
        ssize_t r = msgrcv(msqid, message_ptr, sizeof(message_ptr->msgText), 0, 0);
        //呼叫 System V 的 msgrcv() 從 queue 讀取訊息到 message_ptr
        if (r == -1) { perror("msgrcv"); return; }
        /* ensure null terminated */
        message_ptr->msgText[sizeof(message_ptr->msgText)-1] = '\0'; // 在緩衝最後位置強制放 '\0'
        
    } else if (mailbox_ptr->flag == SHARED_MEM) {
        char *base = mailbox_ptr->storage.shm_addr;
        if (!base) { fprintf(stderr, "receive: invalid shm addr\n"); return; }
        int *status = (int*)base;            // 0 empty,1 full,2 exit
        char *buf = base + sizeof(int); // 讀 status 與資料 buf 的位址
        /* caller guarantees that buffer is full (via semaphores) */
        strncpy(message_ptr->msgText, buf, sizeof(message_ptr->msgText)-1); // 把 shared memory 的 buf 複製到 message_ptr->msgText
        message_ptr->msgText[sizeof(message_ptr->msgText)-1] = '\0';
        /* mark empty for next send */
        *status = 0;
    } else {
        fprintf(stderr, "receive: unknown mailbox flag %d\n", mailbox_ptr->flag);
    }
}


int main(int argc, char **argv){

    /*  TODO: 
        1) Call receive(&message, &mailbox) according to the flow in slide 4
        2) Measure the total receiving time
        3) Get the mechanism from command line arguments
            • e.g. ./receiver 1
        4) Print information on the console according to the output format
        5) If the exit message is received, print the total receiving time and terminate the receiver.c
    */
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mechanism(1=msgq,2=shm)>\n", argv[0]);
        return 1;
    }
    
    int mech = atoi(argv[1]);

    mailbox_t mailbox;
    memset(&mailbox, 0, sizeof(mailbox)); //宣告
    mailbox.flag = mech;

    /* Setup IPC */
    if (mech == MSG_PASSING) {
        key_t key = ftok(FTOK_PATH, 0x66); //建立並 attach 所需 IPC 資源（與 sender 相同的 key）
        if (key == -1) { perror("ftok msg"); return 1; }
        int msqid = msgget(key, IPC_CREAT | 0666);
        if (msqid == -1) { perror("msgget"); return 1; }
        mailbox.storage.msqid = msqid; //將取得的資源 id 或地址填入 mailbox.storage，sender/receiver 會使用相同的 msqid
        printf("Message Passing\n");
    } else if (mech == SHARED_MEM) {
        key_t key = ftok(FTOK_PATH, 0x55); //FTOK_PROJ_SHM:0x55
        if (key == -1) { perror("ftok shm"); return 1; }
        size_t shm_size = sizeof(int) + SHM_TEXT_SIZE;
        int shmid = shmget(key, shm_size, IPC_CREAT | 0666);
        if (shmid == -1) { perror("shmget"); return 1; }
        void *shmaddr = shmat(shmid, NULL, 0);
        if (shmaddr == (void*)-1) { perror("shmat"); return 1; }
        mailbox.storage.shm_addr = (char*)shmaddr;
        printf("Shared Memory\n");
    } else {
        fprintf(stderr, "Unknown mechanism %d\n", mech);
        return 1;
    }

    /* 開啟或建立兩個 named semaphore */
    int sender_created = 0, receiver_created = 0;
    sem_t *sem_sender = open_or_create_sem(SEM_SENDER_NAME, 1, &sender_created);
    if (sem_sender == SEM_FAILED) return 1;
    sem_t *sem_receiver = open_or_create_sem(SEM_RECEIVER_NAME, 0, &receiver_created);
    /* receiver 會等待 sem_receiver，並在接收完後 sem_post(sem_sender) */
    if (sem_receiver == SEM_FAILED) {
        sem_close(sem_sender);
        return 1;
    }

    /*
    // Open semaphores (created/unlinked by sender when starts) 
    sem_t *sem_sender = sem_open(SEM_SENDER_NAME, O_CREAT, 0666, 1);
    sem_t *sem_receiver = sem_open(SEM_RECEIVER_NAME, O_CREAT, 0666, 0);
    if (sem_sender == SEM_FAILED || sem_receiver == SEM_FAILED) {
        perror("sem_open");
        return 1;
    }
    */
    
    struct timespec start, end; //計時
    double total_comm_time = 0.0;

    message_t recv;
    recv.mType = 0; //宣告接收用的 message 結構。mType 對 System V message queue 用途不同（msgrcv 可用 mtype），此處設定為 0 表示接受任何 type。

    while (1) { //無限等待直到處理完訊息
        /* wait until sender posts (not timed) */
        sem_wait(sem_receiver); //等待 sender 喚醒，否則阻塞

        // 計算傳送
        clock_gettime(CLOCK_MONOTONIC, &start);
        receive(&recv, &mailbox); //實際接收
        clock_gettime(CLOCK_MONOTONIC, &end);
        total_comm_time += (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9;

        /* printing not timed */
        if (strcmp(recv.msgText, EXIT_MARKER) == 0) { // 接收到sender的exit訊號
            printf("Sender exit!\n");
            /* post sender as safety (sender probably already finished) */
            sem_post(sem_sender); // 喚醒 sender 發下一筆
            break;
        } else {
            printf("Receiving message:\t%s\n", recv.msgText);
            /* after consuming, unblock sender (not timed) */
            sem_post(sem_sender); // 喚醒 sender 發下一筆
        }
    }

    printf("Total time taken in receiving msg: %.6f s\n", total_comm_time);

    /* cleanup: close/unlink semaphores and remove IPC */
    sem_close(sem_sender);
    sem_close(sem_receiver);
    sem_unlink(SEM_SENDER_NAME); //移除named semaphore
    sem_unlink(SEM_RECEIVER_NAME);

    if (mech == SHARED_MEM) { // 清理 IPC 資源
        void *addr = (void*)mailbox.storage.shm_addr;
        shmdt(addr); //內存脫離
        key_t key = ftok(FTOK_PATH, 0x55);
        if (key != -1) {
            int shmid = shmget(key, sizeof(int)+SHM_TEXT_SIZE, 0666);
            if (shmid != -1) shmctl(shmid, IPC_RMID, NULL);
        }
    } else if (mech == MSG_PASSING) {
        int msqid = mailbox.storage.msqid;
        if (msqid >= 0) msgctl(msqid, IPC_RMID, NULL);
    }

    return 0;
}

//兩個 semaphore（sem_sender、sem_receiver）能清楚表示「誰該在此輪運作」：只有 sem_sender>0 時 sender 才能送；只有 sem_receiver>0 時 receiver 才能接。
