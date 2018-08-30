#include <fcntl.h>
#include <numa.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <jemalloc/jemalloc.h>

#include "high.h"
#include "sicm_low.h"
#include "sicmimpl.h"
#include "profile.h"

static struct sicm_device_list device_list;
static enum sicm_device_tag default_device_tag;
static struct sicm_device *default_device;

/* For profiling */
static int should_profile;
int max_sample_pages;
int sample_freq;

/* Keep track of all extents */
extent_arr *extents;

/* Keeps track of arenas */
arena_info **arenas;
static enum arena_layout layout;
static int max_arenas, arenas_per_thread;
int max_index;

/* Associates a thread with an index (starting at 0) into the `arenas` array */
static pthread_key_t thread_key;
static int *thread_indices, *orig_thread_indices, *max_thread_indices, max_threads;

/* Passes an arena index to the extent hooks */
static int *pending_indices;

/* Takes a string as input and outputs which arena layout it is */
enum arena_layout parse_layout(char *env) {
	size_t max_chars;

	max_chars = 32;

	if(strncmp(env, "SHARED_ONE_ARENA", max_chars) == 0) {
		return SHARED_ONE_ARENA;
	} else if(strncmp(env, "EXCLUSIVE_ONE_ARENA", max_chars) == 0) {
		return EXCLUSIVE_ONE_ARENA;
	} else if(strncmp(env, "SHARED_TWO_ARENAS", max_chars) == 0) {
		return SHARED_TWO_ARENAS;
	} else if(strncmp(env, "EXCLUSIVE_TWO_ARENAS", max_chars) == 0) {
		return EXCLUSIVE_TWO_ARENAS;
	} else if(strncmp(env, "SHARED_SITE_ARENAS", max_chars) == 0) {
		return SHARED_SITE_ARENAS;
	} else if(strncmp(env, "EXCLUSIVE_SITE_ARENAS", max_chars) == 0) {
		return EXCLUSIVE_SITE_ARENAS;
	}

  return INVALID_LAYOUT;
}

/* Converts an arena_layout to a string */
char *layout_str(enum arena_layout layout) {
  switch(layout) {
    case SHARED_ONE_ARENA:
      return "SHARED_ONE_ARENA";
    case EXCLUSIVE_ONE_ARENA:
      return "EXCLUSIVE_ONE_ARENA";
    case SHARED_TWO_ARENAS:
      return "SHARED_TWO_ARENAS";
    case EXCLUSIVE_TWO_ARENAS:
      return "EXCLUSIVE_TWO_ARENAS";
    case SHARED_SITE_ARENAS:
      return "SHARED_SITE_ARENAS";
    case EXCLUSIVE_SITE_ARENAS:
      return "EXCLUSIVE_SITE_ARENAS";
    default:
      break;
  }

  return "INVALID_LAYOUT";
}

/* Gets environment variables and sets up globals */
void set_options() {
  char *env, *endptr;
  long long tmp_val;
  struct sicm_device *device;
  int i;

  /* Get the arena layout */
  env = getenv("SH_ARENA_LAYOUT");
  if(env) {
    layout = parse_layout(env);
  } else {
    layout = DEFAULT_ARENA_LAYOUT;
  }
  printf("Arena layout: %s\n", layout_str(layout));

  /* Get max_threads */
  max_threads = numa_num_possible_cpus();
  env = getenv("SH_MAX_THREADS");
  if(env) {
    endptr = NULL;
    tmp_val = strtoimax(env, &endptr, 10);
    if((tmp_val == 0) || (tmp_val > INT_MAX)) {
      printf("Invalid thread number given. Defaulting to %d.\n", max_threads);
    } else {
      max_threads = (int) tmp_val;
    }
  }
  printf("Maximum threads: %d\n", max_threads);

  /* Get max_arenas.
   * Keep in mind that 4096 is the maximum number supported by jemalloc.
   * An error occurs if this limit is reached.
   */
  max_arenas = 4096;
  env = getenv("SH_MAX_ARENAS");
  if(env) {
    endptr = NULL;
    tmp_val = strtoimax(env, &endptr, 10);
    if((tmp_val == 0) || (tmp_val > INT_MAX)) {
      printf("Invalid arena number given. Defaulting to %d.\n", max_arenas);
    } else {
      max_arenas = (int) tmp_val;
    }
  }
  printf("Maximum arenas: %d\n", max_arenas);

  /* Should we profile? */
  env = getenv("SH_PROFILING");
  should_profile = 0;
  if(env) {
    should_profile = 1;
    printf("Profiling.\n");
  }

  /* What sample frequency should we use? Default is 2048. Higher
   * frequencies will fill up the sample pages (below) faster.
   */
  env = getenv("SH_SAMPLE_FREQ");
  sample_freq = 2048;
  if(env) {
    endptr = NULL;
    tmp_val = strtoimax(env, &endptr, 10);
    if((tmp_val <= 0)) {
      printf("Invalid sample frequency given. Defaulting to %d.\n", sample_freq);
    } else {
      sample_freq = (int) tmp_val;
    }
  }
  printf("Sample frequency: %d\n", sample_freq);

  /* How many samples should be collected by perf, maximum?
   * Assuming we're only tracking addresses, this number is multiplied by 
   * the page size and divided by 16 to get the maximum number of samples.
   * 8 of those bytes are the header, and the other 8 are the address itself.
   * By default this is 64 pages, which yields 16k samples.
   */
  env = getenv("SH_MAX_SAMPLE_PAGES");
  max_sample_pages = 64;
  if(env) {
    endptr = NULL;
    tmp_val = strtoimax(env, &endptr, 10);
    /* Value needs to be non-negative, less than or equal to 512, and a power of 2. */
    if((tmp_val <= 0) || (tmp_val > 512) || (tmp_val & (tmp_val - 1))) {
      printf("Invalid number of pages given (%d). Defaulting to %d.\n", tmp_val, max_sample_pages);
    } else {
      max_sample_pages = (int) tmp_val;
    }
  }
  printf("Maximum sample pages: %d\n", max_sample_pages);

  /* Get default_device_tag */
  env = getenv("SH_DEFAULT_DEVICE");
  if(env) {
    default_device_tag = sicm_get_device_tag(env);
  } else {
    default_device_tag = INVALID_TAG;
  }
  printf("Default device tag: %s.\n", sicm_device_tag_str(default_device_tag));
  
  /* Get default_device */
  if(default_device_tag == INVALID_TAG) {
    default_device = device_list.devices;
  } else {
    device = device_list.devices;
    for(i = 0; i < device_list.count; i++) {
      if(device->tag == default_device_tag) {
        break;
      }
      device++;
    }
    if(device == device_list.devices + device_list.count) {
      device = device_list.devices;
    }
    default_device = device;
  }
  printf("Default device: %s\n", sicm_device_tag_str(default_device->tag));

  /* Get arenas_per_thread */
  switch(layout) {
    case SHARED_ONE_ARENA:
    case EXCLUSIVE_ONE_ARENA:
      arenas_per_thread = 1;
      break;
    case SHARED_TWO_ARENAS:
    case EXCLUSIVE_TWO_ARENAS:
      arenas_per_thread = 2;
      break;
    case SHARED_SITE_ARENAS:
    case EXCLUSIVE_SITE_ARENAS:
      arenas_per_thread = max_arenas;
      break;
    default:
      arenas_per_thread = 1;
      break;
  };
  printf("Arenas per thread: %d\n", arenas_per_thread);
}

int get_thread_index() {
  int *val;

  /* Get this thread's index */
  val = (int *) pthread_getspecific(thread_key);

  /* If nonexistent, increment the counter and set it */
  if(val == NULL) {
    if(thread_indices + 1 >= max_thread_indices) {
      fprintf(stderr, "Maximum number of threads reached. Aborting.\n");
      exit(1);
    }
    pthread_setspecific(thread_key, (void *) thread_indices);
    val = thread_indices;
    thread_indices++;
  }

  return *val;
}

/* Adds an arena to the `arenas` array. */
void sh_create_arena(int index, int id) {

  if(index > (max_arenas - 1)) {
    fprintf(stderr, "Maximum number of arenas reached. Aborting.\n");
    exit(1);
  }

  /* Put an upper bound on the indices that need to be searched */
  if(index > max_index) {
    max_index = index;
  }

  /* Create the arena if it doesn't exist */
  if(arenas[index] == NULL) {
    arenas[index] = calloc(1, sizeof(arena_info));
    arenas[index]->index = index;
    arenas[index]->accesses = 0;
    arenas[index]->id = id;
    arenas[index]->rss = 0;
    arenas[index]->peak_rss = 0;
    arenas[index]->arena = sicm_arena_create(0, default_device);
  }
}

/* Adds a extent to the `extents` array. */
void sh_create_extent(void *start, void *end) {
  int thread_index, arena_index;

  /* Get this thread's current arena index from `pending_indices` */
  thread_index = get_thread_index();
  arena_index = pending_indices[thread_index];

  /* A extent allocation is happening without an sh_alloc... */
  if(arena_index == 0) {
    fprintf(stderr, "Unknown extent allocation. Aborting.\n");
    exit(1);
  }

  extent_arr_insert(extents, start, end, arenas[arena_index]);
}

/* Gets the index that the ID should go into */
int get_arena_index(int id) {
  int ret, thread_index;

  thread_index = get_thread_index();

  ret = 0;
  switch(layout) {
    case SHARED_ONE_ARENA:
      ret = 0;
      break;
    case EXCLUSIVE_ONE_ARENA:
      ret = thread_index;
      break;
    case SHARED_TWO_ARENAS:
      /* Should have one hot and one cold arena */
      break;
    case EXCLUSIVE_TWO_ARENAS:
      /* Same */
      break;
    case SHARED_SITE_ARENAS:
      ret = id;
      break;
    case EXCLUSIVE_SITE_ARENAS:
      ret = (thread_index * arenas_per_thread) + id;
      break;
    default:
      fprintf(stderr, "Invalid arena layout. Aborting.\n");
      exit(1);
      break;
  };

  pending_indices[thread_index] = ret;
  sh_create_arena(ret, id);

  return ret;
}

void* sh_realloc(int id, void *ptr, size_t sz) {
  int index;

  if(layout == INVALID_LAYOUT) {
    return realloc(ptr, sz);
  }

  index = get_arena_index(id);
  return sicm_arena_realloc(arenas[index]->arena, ptr, sz);
}

/* Accepts an allocation site ID and a size, does the allocation */
void* sh_alloc(int id, size_t sz) {
  int index;
  void *ret;

  if((layout == INVALID_LAYOUT) || !sz) {
    return je_malloc(sz);
  }

  index = get_arena_index(id);
  ret = sicm_arena_alloc(arenas[index]->arena, sz);
  return ret;
}

void sh_free(void* ptr) {
  if(layout == INVALID_LAYOUT) {
    je_free(ptr);
  } else {
    sicm_free(ptr);
  }
}

__attribute__((constructor))
void sh_init() {
  int i;

  device_list = sicm_init();
  set_options();
  
  if(layout != INVALID_LAYOUT) {
    /* `arenas` is a pseudo-two-dimensional array, first dimension is per-thread */
    /* Second dimension is one for each arena that each thread will have */
    arenas = (arena_info **) calloc(max_threads * arenas_per_thread, sizeof(arena_info *));

    extents = extent_arr_init();

    /* Stores the index into the `arenas` array for each thread */
    pthread_key_create(&thread_key, NULL);
    thread_indices = (int *) malloc(max_threads * sizeof(int));
    orig_thread_indices = thread_indices;
    max_thread_indices = orig_thread_indices + max_threads;
    for(i = 0; i < max_threads; i++) {
      thread_indices[i] = i;
    }
    pthread_setspecific(thread_key, (void *) thread_indices);
    thread_indices++;

    /* Stores an index into `arenas` for the extent hooks */
    pending_indices = (int *) calloc(max_threads, sizeof(int));

    /* Set the arena allocator's callback function */
    sicm_extent_alloc_callback = &sh_create_extent;

    if(should_profile) {
      sh_start_profile_thread();
    }
  }
}

__attribute__((destructor))
void sh_terminate() {
  size_t i;

  /* Clean up the low-level interface */
  sicm_fini(&device_list);

  if(layout != INVALID_LAYOUT) {

    /* Clean up the profiler */
    if(should_profile) {
      sh_stop_profile_thread();
    }

    /* Clean up the arenas */
    for(i = 0; i <= max_index; i++) {
      if(!arenas[i]) continue;
      sicm_arena_destroy(arenas[i]->arena);
      free(arenas[i]);
    }
    free(arenas);

    free(orig_thread_indices);
    extent_arr_free(extents);
  }
}