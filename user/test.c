#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int main(int argc, char** argv){
    int n_forks = 5;
    //printf("\n\n\n %d \n\n\n", cpu_process_count(0));

    for (int i = 0; i < n_forks; i++) {
    	fork();
    }
    for(int i=0 ; i<50 ; i++){
        printf(  "%d. my PID  -->  %d\n", i, getpid());
        if(i == 25){
            

            // printf("cpu 0 = %d\n", cpu_process_count(0));
            // printf("cpu 1 = %d\n", cpu_process_count(1));

            // printf("cpu 2 = %d\n", cpu_process_count(2));

        }
    }

    printf("\n\n\n %d \n\n\n", cpu_process_count(0));

    
    //    for (int i = 0; i < n_forks; i++) {
    // 	fork();
       
    // }
    //  printf("PID - %d\n", getpid());

   // get_cpu();
    exit(0);
}
