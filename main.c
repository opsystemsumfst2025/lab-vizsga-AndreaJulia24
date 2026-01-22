#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define NUM_TRADERS 3
#define BUFFER_SIZE 10
#define INITIAL_BALANCE 10000.0

typedef struct Transaction{
	char type[10];
	char stock[10];
	int quantity;
	double price;
	struct Transaction* next;
}Transaction;

typedef struct StockPrice{
	char stock[10];
	double price;
}StockPrice;


// TODO: Globális változók
static  StockPrice price_buffer[BUFFER_SIZE];
static  int buffer_count = 0;
static int buffer_read_idx = 0;
static int buffer_write_idx = 0;
static double wallet_balance = INITIAL_BALANCE;
static  int stocks_owned = 0;
// - mutex-ek (wallet, buffer, transaction)
static pthread_mutex_t wallet_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t buffer_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t transaction_mutex=PTHREAD_MUTEX_INITIALIZER;
// - condition variable
static pthread_cond_t buffer_cond=PTHREAD_COND_INITIALIZER; //race condition elkerulese miatt 
// - transaction_head pointer
static Transaction* transaction_head=NULL;
// - running flag (volatile sig_atomic_t)
static volatile sig_atomic_t running = 1;
// - market_pid
static pid_t market_pid;

static void die(const char *msg){
	perror(msg);
	exit(EXIT_FAILURE);
}

// TODO: Implementáld az add_transaction függvényt
// 4.ALPONT: malloc hasznalata a lancolt listahoz
static void add_transaction(const char* type,const char* stock,int quant, double price)
{
     Transaction *new_trans=malloc(sizeof(Transaction));
     if(!new_trans){
     	return;
     }
     strncpy(new_trans->type,type,10);
     new_trans->type[10] = '\0';
     strncpy(new_trans->stock,stock,10);
     new_trans->stock[10] = '\0';
     new_trans->quantity=quant;
     new_trans->price=price;

     pthread_mutex_lock(&transaction_mutex);
     new_trans->next=transaction_head;
     transaction_head=new_trans;
     pthread_mutex_unlock(&transaction_mutex);
}

// TODO: Implementáld a print_transactions függvényt
// tranzakcio naplozasa -vegehez
static void print_transactions(){
	printf("\n===Tranzakcios Naplo===\n");

	pthread_mutex_lock(&transaction_mutex);
	
	Transaction *current=transaction_head;
	int count=0;
	
	if(current == NULL){
	   printf("A naplo ures - nem tortent kereskedes\n");
	}

	while(current != NULL){
	   printf("[%s] %-5s: %d db | %.2f $\n",current->type, current->stock,
	   current->quantity, current->price);
	   current=current->next;
	   count++;
	}
   	 pthread_mutex_unlock(&transaction_mutex);
   
	 printf("==============================================\n");
	 printf("Osszesen %d tranzakcio kerult rogzitesre.\n",count);
	 printf("==============================================\n");
}

// TODO: Implementáld a free_transactions függvényt
// Ez kell a Valgrind tiszta kimenethez!
//4.ALPONT: memoria felszabaditasa
static void free_transactions(){
	pthread_mutex_lock(&transaction_mutex);
	Transaction* current=transaction_head;
	while(current){
	   Transaction *temp=current;
	   current=current->next;
	   free(temp);
	}
   transaction_head=NULL;
   pthread_mutex_unlock(&transaction_mutex);
}

// TODO: Signal handler (SIGINT)
//  5.ALPONT: CTRL+C beallitasa  
static void  signal_handler(int sig){
	(void)sig;
	running = 0;
	if(market_pid > 0){
	  kill(market_pid,SIGTERM);
	}
    pthread_cond_broadcast(&buffer_cond);
}

// TODO: Piac folyamat függvénye
// Végtelen ciklusban:
// - Generálj random részvénynevet és árat
// - Írás a pipe_fd-re (write)
// - sleep(1)
static void market_process(int pipe_write_fd){
	const char* stocks[]={"AAPL","GOOG","TSLA","MSFT"};
	srand(time(NULL) ^ getpid());
	
	printf("[PIAC] Gyerek folyamat elindult (PID: %d)\n",getpid());

	while(1){
	   char message[64];
	   const char* s = stocks[rand() % 4];
	   int p = 100 + (rand() % 900); //random generalt price-ok
	   
	   int len=snprintf(message,sizeof(message),"%s %d",s,p); // Formatum
	   if(write(pipe_write_fd,message,len+1) == -1 ){ //iras a pipe-ba
	   	break;
           }
	 printf("[PIAC] arfolyam kuldve: %s %d $\n",s,p);
	 sleep(1);
      }
 }


// TODO: Kereskedő szál függvénye ------2.KERESKEDO MOTOR(PTHREAD ES MUTEX)
static void* trader_thread(void* arg){
	int id=*(int*)arg;
	free(arg);	
	
	printf("[TRADER %d] Szal elindult es var...\n",id);

	while(1){
	    StockPrice current_data;
	    pthread_mutex_lock(&buffer_mutex);
	    
           //3. ALPONT: condition variable varakozas(nem busy-wait)	
	    while(buffer_count == 0 && running){ //var,amig nincs adat
		pthread_cond_wait(&buffer_cond,&buffer_mutex);
	    }
	    if(!running && buffer_count == 0){ //ha leall es nincs tobb adat
		pthread_mutex_unlock(&buffer_mutex);
		break;
	    }
	
	//adat kivetele a pufferbol 
	current_data=price_buffer[buffer_read_idx];
	buffer_read_idx=(buffer_read_idx + 1) % BUFFER_SIZE;
	buffer_count--;	
	pthread_mutex_unlock(&buffer_mutex); //elengedjuk
	
	// 2.ALPONT: KRITIKUS SZAKASZ: wallet_balance es stock_owned
	//Vedelem--kritikus szakasz
	pthread_mutex_lock(&wallet_mutex);
	
	//Strategia
	if(wallet_balance >= current_data.price){
		wallet_balance -= current_data.price;
		stocks_owned++;
		printf("[Trader %d] VETEL: %s | %2.f $ |Egyenleg: %2.f $\n", id, 
		current_data.stock, current_data.price, wallet_balance);
		add_transaction("VETEL",current_data.stock,1,current_data.price);
	}
	else if(stocks_owned > 0){
		wallet_balance += current_data.price;
		stocks_owned--;
		printf("[Trader %d] ELADAS: %s | %2.f $ |Egyenleg: %2.f $\n", id,
		current_data.stock,current_data.price, wallet_balance);
		add_transaction("ELADAS",current_data.stock,1,current_data.price);
	}
	pthread_mutex_unlock(&wallet_mutex);
	//fflush(stdout); //azonnali terminal kiiras kenyszeritese
	usleep(100000);
      }
  printf("[Trader %d] Szal kilep.\n",id);
  return NULL;
}

int main() {
	
    setvbuf(stdout,NULL,_IONBF,0); //nincs puffereles

    int pipe_fd[2];
    pthread_t traders[NUM_TRADERS];
    
    printf("========================================\n");
    printf("  WALL STREET - PARHUZAMOS TOZSDE\n");
    printf("========================================\n");
    printf("Kezdo egyenleg: %.2f $\n", INITIAL_BALANCE);
    printf("Kereskedok szama: %d\n", NUM_TRADERS);
    printf("Ctrl+C a leallitashoz\n");
    printf("========================================\n\n");
   // fflush(stdout);
    
    // TODO: Signal handler regisztrálása
     if(signal(SIGINT,signal_handler)==SIG_ERR){
   	die("Signal hiba"); 
   }
    
    // TODO: Pipe létrehozasa
    if( pipe(pipe_fd)==-1){
    	die("pipe() hiba");
	return 1;
    }
    
    // TODO: Fork - Piac folyamat indítása ---1.LEPES
     market_pid = fork();
    if(market_pid < 0){
    	die("fork() hiba");
    }
    if(market_pid == 0){ //ha a gyerek == 0 -> market process
    	close(pipe_fd[0]);
	market_process(pipe_fd[1]);
	_exit(EXIT_SUCCESS);
    }
    else{
    	close(pipe_fd[1]); //iro ag lezarasa a szuloben
    }
 
    // TODO: Kereskedő szálak indítása (pthread_create)
    for(int i=0;i<NUM_TRADERS;i++){
	int *id=malloc(sizeof(int));
	if(!id){
	 die("Malloc hiba");
	}
        *id = i + 1;
   	if(pthread_create(&traders[i],NULL,trader_thread,id) != 0){
	      die("Szal hiba");
	}  
    }
    
    // TODO: Master ciklust
    char read_buff[128];
    while(1){
    	 ssize_t n=read(pipe_fd[0],read_buff,sizeof(read_buff) - 1);
    	 if(n<=0){
   	   break; //csak akkor lepunk ki ha a pipe lezarult
   	 }
   
	 read_buff[n] = '\0';
	 StockPrice incoming; 
	 if(sscanf(read_buff, "%s %lf" , incoming.stock, &incoming.price) == 2){
	
	//master visszajelzes --- a szulo megkapja az arat
	printf("[MASTER] Adat erkezett: %s %.2f $\n",incoming.stock,incoming.price);
	//fflush(stdout);
		pthread_mutex_lock(&buffer_mutex);
		
		if(buffer_count < BUFFER_SIZE){
		     price_buffer[buffer_write_idx] = incoming;
		     buffer_write_idx = (buffer_write_idx + 1) % BUFFER_SIZE;
		     buffer_count++;

		     pthread_cond_broadcast(&buffer_cond);
		}
	   pthread_mutex_unlock(&buffer_mutex);
	 }
   }
    
    // TODO: Cleanup + vegso kiirasok
   //egy utolso jelzes a szalaknak hogy biztosra eszrevegyek a leallast
   pthread_mutex_lock(&buffer_mutex);
   pthread_cond_broadcast(&buffer_cond);
   pthread_mutex_unlock(&buffer_mutex);
   
   //kereskedo szalak bevarasa
   for(int i=0;i<NUM_TRADERS;i++){
    pthread_join(traders[i], NULL);
   }
   
   //gyerek folyamat bevarasa
   waitpid(market_pid,NULL,0);
   close(pipe_fd[0]);
   
   printf("\n====================================\n");
   printf("\n========VEGLEGES EGYENLEG=============\n");
   printf("Penz egyenleg: %2.f $\n", wallet_balance);
   printf("Reszveny keszlet: %d db\n", stocks_owned);
   print_transactions(); //naplozas

   printf("\n=======================================\n");

   free_transactions();
   
   pthread_mutex_destroy(&wallet_mutex);
   pthread_mutex_destroy(&buffer_mutex);
   pthread_mutex_destroy(&transaction_mutex);
   pthread_cond_destroy(&buffer_cond);

   printf("\n[RENDSZER] Sikeres leallitas.\n");
   return 0;
}

