#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "sdf.h"

#ifdef PARALLEL
#include <mpi.h>
#endif

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#define ABS(a) (((a) > 0) ? (a) : (-(a)))


#define SDF_MAX_RND (1LL<<32)

static uint32_t sdf_random(void);
static void sdf_random_init(void);


int sdf_fopen(sdf_file_t *h)
{
    int ret = 0;

#ifdef PARALLEL
    ret = MPI_File_open(h->comm, (char*)h->filename, MPI_MODE_RDONLY,
        MPI_INFO_NULL, &h->filehandle);
    if (ret) h->filehandle = 0;
#else
    h->filehandle = fopen(h->filename, "r");
#endif
    if (!h->filehandle) ret = 1;

    return ret;
}


sdf_file_t *sdf_open(const char *filename, comm_t comm, int mode, int use_mmap)
{
    sdf_file_t *h;
    int ret;

    // Create filehandle
    h = malloc(sizeof(*h));
    memset(h, 0, sizeof(*h));

#ifdef SDF_DEBUG
    h->dbg_count = DBG_CHUNK;
    h->dbg = h->dbg_buf = malloc(h->dbg_count);
#endif
    h->string_length = 64;
    h->indent = 0;

    h->done_header = 0;
    h->ncpus = 1;
    h->use_summary = 1;
    h->sdf_lib_version  = SDF_LIB_VERSION;
    h->sdf_lib_revision = SDF_LIB_REVISION;

#ifdef PARALLEL
    h->comm = comm;
    MPI_Comm_rank(h->comm, &h->rank);
#else
    h->rank = 0;
#endif
    h->filename = malloc(strlen(filename)+1);
    memcpy(h->filename, filename, strlen(filename)+1);

    sdf_fopen(h);
    if (!h->filehandle) {
        free(h->filename);
        free(h);
        h = NULL;
        return h;
    }

#ifndef PARALLEL
    if (use_mmap)
        h->mmap = "";
    else
#endif
        h->mmap = NULL;

    ret = sdf_read_header(h);
    if (ret) {
        h = NULL;
        return h;
    }

#ifndef PARALLEL
    if (h->mmap)
        h->mmap = mmap(NULL, h->summary_location, PROT_READ, MAP_SHARED,
            fileno(h->filehandle), 0);
#endif

    return h;
}


#define FREE_ARRAY(value) do { \
    if (value) { \
        int i; \
        if (b->n_ids) { \
            for (i = 0; i < b->n_ids; i++) \
                if (value[i]) free(value[i]); \
        } else { \
            for (i = 0; i < b->ndims; i++) \
                if (value[i]) free(value[i]); \
        } \
        free(value); \
    }} while(0)


static int sdf_free_block_data(sdf_file_t *h, sdf_block_t *b)
{
    int i;

    if (!b) return 1;

    if (b->grids) {
        if (!h->mmap && b->done_data)
            for (i = 0; i < b->ndims; i++) if (b->grids[i]) free(b->grids[i]);
        free(b->grids);
    }
    if (!h->mmap && b->data && b->done_data && !b->dont_own_data) {
        free(b->data);
    }
    if (b->node_list) free(b->node_list);
    if (b->boundary_cells) free(b->boundary_cells);
    b->node_list = NULL;
    b->boundary_cells = NULL;
    b->done_data = 0;
    b->grids = NULL;
    b->data = NULL;

    return 0;
}


static int sdf_free_block(sdf_file_t *h, sdf_block_t *b)
{
    if (!b) return 1;

    if (b->id) free(b->id);
    if (b->units) free(b->units);
    if (b->mesh_id) free(b->mesh_id);
    if (b->material_id) free(b->material_id);
    if (b->name) free(b->name);
    if (b->material_name) free(b->material_name);
    if (b->dims_in) free(b->dims_in);
    if (b->dim_mults) free(b->dim_mults);
    if (b->extents) free(b->extents);
    FREE_ARRAY(b->variable_ids);
    FREE_ARRAY(b->material_names);
    sdf_free_block_data(h, b);

    free(b);
    b = NULL;

    return 0;
}



int sdf_free_blocklist_data(sdf_file_t *h)
{
    sdf_block_t *b, *next;
    int i;

    if (!h || !h->filehandle) return 1;

    // Destroy blocklist
    if (h->blocklist) {
        b = h->blocklist;
        for (i=0; i < h->nblocks; i++) {
            next = b->next;
            sdf_free_block_data(h, b);
            b = next;
        }
    }

    return 0;
}



static int sdf_free_handle(sdf_file_t *h)
{
    sdf_block_t *b, *next;
    int i;

    if (!h || !h->filehandle) return 1;

    // Destroy blocklist
    if (h->blocklist) {
        b = h->blocklist;
        for (i=0; i < h->nblocks; i++) {
            if (!b->next) break;
            next = b->next;
            sdf_free_block(h, b);
            b = next;
        }
    }
    // Destroy handle
    if (h->buffer) free(h->buffer);
    if (h->code_name) free(h->code_name);
    if (h->filename) free(h->filename);
    memset(h, 0, sizeof(sdf_file_t));
    free(h);
    h = NULL;

    return 0;
}



int sdf_fclose(sdf_file_t *h)
{
    // No open file
    if (!h || !h->filehandle) return 1;

#ifdef PARALLEL
    MPI_Barrier(h->comm);

    MPI_File_close(&h->filehandle);
#else
    fclose(h->filehandle);
#endif
    h->filehandle = 0;

    return 0;
}



int sdf_close(sdf_file_t *h)
{
    // No open file
    if (!h || !h->filehandle) return 1;

    sdf_fclose(h);

    // Destroy filehandle
    sdf_free_handle(h);

    return 0;
}



int sdf_set_rank_master(sdf_file_t *h, int rank)
{
    if (h)
        h->rank_master = rank;
    else
        return -1;

    return 0;
}



int sdf_read_nblocks(sdf_file_t *h)
{
    if (h)
        return h->nblocks;
    else
        return -1;
}



int sdf_set_ncpus(sdf_file_t *h, int ncpus)
{
    if (!h) return -1;

    h->ncpus = ncpus;
    return 0;
}



/*
int sdf_read_jobid(sdf_file_t *h, sdf_jobid_t *jobid)
{
    if (h && jobid)
        memcpy(jobid, &h->jobid, sizeof(jobid));
    else
        return -1;

    return 0;
}
*/



#ifdef PARALLEL
static int factor2d(int ncpus, uint64_t *dims, int *cpu_split)
{
    const int ndims = 2;
    int dmin[ndims], npoint_min[ndims], cpu_split_tmp[ndims], grids[ndims][2];
    int i, j, ii, jj, n, cpus, maxcpus, grid, split_big;
    float gridav, deviation, mindeviation;

    cpus = 1;
    gridav = 1;
    for (i=0; i < ndims; i++) {
        dmin[i] = MIN(ncpus, dims[i]);
        cpus = cpus * dmin[i];
        gridav = gridav * dims[i];
    }
    mindeviation = gridav;
    gridav = gridav / ncpus;

    maxcpus = MIN(ncpus,cpus);

    for (j=0; j < dmin[1]; j++) {
        cpu_split_tmp[1] = dmin[1]-j;
    for (i=0; i < dmin[0]; i++) {
        cpu_split_tmp[0] = dmin[0]-i;

        cpus = 1;
        for (n=0; n < ndims; n++)
            cpus = cpus * cpu_split_tmp[n];

        if (cpus != maxcpus) continue;

        for (n=0; n < ndims; n++) {
            npoint_min[n] = dims[n] / cpu_split_tmp[n];
            split_big = dims[n] - cpu_split_tmp[n] * npoint_min[n];
            grids[n][0] = npoint_min[n];
            grids[n][1] = npoint_min[n] + 1;
            if (cpu_split_tmp[n] == split_big) grids[n][0] = 0;
            if (split_big == 0) grids[n][1] = 0;
        }

        for (ii=0; ii < 2; ii++) {
        for (jj=0; jj < 2; jj++) {
            grid = grids[0][ii] * grids[1][jj];
            deviation = ABS(grid-gridav);
            if (deviation < mindeviation) {
              mindeviation = deviation;
              for (n=0; n < ndims; n++)
                  cpu_split[n] = cpu_split_tmp[n];
            }
        }}
    }}

    return 0;
}



static int factor3d(int ncpus, uint64_t *dims, int *cpu_split)
{
    const int ndims = 3;
    int dmin[ndims], npoint_min[ndims], cpu_split_tmp[ndims], grids[ndims][2];
    int i, j, k, ii, jj, kk, n, cpus, maxcpus, grid, split_big;
    float gridav, deviation, mindeviation;

    cpus = 1;
    gridav = 1;
    for (i=0; i < ndims; i++) {
        dmin[i] = MIN(ncpus, dims[i]);
        cpus = cpus * dmin[i];
        gridav = gridav * dims[i];
    }
    mindeviation = gridav;
    gridav = gridav / ncpus;

    maxcpus = MIN(ncpus,cpus);

    for (k=0; k < dmin[2]; k++) {
        cpu_split_tmp[2] = dmin[2]-k;
    for (j=0; j < dmin[1]; j++) {
        cpu_split_tmp[1] = dmin[1]-j;
    for (i=0; i < dmin[0]; i++) {
        cpu_split_tmp[0] = dmin[0]-i;

        cpus = 1;
        for (n=0; n < ndims; n++)
            cpus = cpus * cpu_split_tmp[n];

        if (cpus != maxcpus) continue;

        for (n=0; n < ndims; n++) {
            npoint_min[n] = dims[n] / cpu_split_tmp[n];
            split_big = dims[n] - cpu_split_tmp[n] * npoint_min[n];
            grids[n][0] = npoint_min[n];
            grids[n][1] = npoint_min[n] + 1;
            if (cpu_split_tmp[n] == split_big) grids[n][0] = 0;
            if (split_big == 0) grids[n][1] = 0;
        }

        for (ii=0; ii < 2; ii++) {
        for (jj=0; jj < 2; jj++) {
        for (kk=0; kk < 2; kk++) {
            grid = grids[0][ii] * grids[1][jj] * grids[2][kk];
            deviation = ABS(grid-gridav);
            if (deviation < mindeviation) {
              mindeviation = deviation;
              for (n=0; n < ndims; n++)
                  cpu_split[n] = cpu_split_tmp[n];
            }
        }}}
    }}}

    return 0;
}
#endif



int sdf_get_domain_extents(sdf_file_t *h, int rank, int *start, int *local)
{
    sdf_block_t *b = h->current_block;
    int n;
#ifdef PARALLEL
    int npoint_min, split_big, coords, div;

    if (b->stagger != SDF_STAGGER_CELL_CENTRE)
        for (n = 0; n < b->ndims; n++) b->dims[n]--;

    memset(start, 0, 3*sizeof(int));

    div = 1;
    for (n = 0; n < b->ndims; n++) {
        coords = (rank / div) % b->cpu_split[n];

        if (coords == 0)
            b->proc_min[n] = MPI_PROC_NULL;
        else
            b->proc_min[n] = rank - div;

        if (coords == b->cpu_split[n] - 1)
            b->proc_max[n] = MPI_PROC_NULL;
        else
            b->proc_max[n] = rank + div;

        div = div * b->cpu_split[n];
        npoint_min = b->dims[n] / b->cpu_split[n];
        split_big = b->dims[n] - b->cpu_split[n] * npoint_min;
        if (coords >= split_big) {
            start[n] = split_big * (npoint_min + 1)
                + (coords - split_big) * npoint_min;
            local[n] = npoint_min;
        } else {
            start[n] = coords * (npoint_min + 1);
            local[n] = npoint_min + 1;
        }
    }

    if (b->stagger != SDF_STAGGER_CELL_CENTRE) {
        for (n = 0; n < b->ndims; n++) {
            b->dims[n]++;
            local[n]++;
        }
    }
#else
    memset(start, 0, 3*sizeof(int));
    for (n=0; n < b->ndims; n++) local[n] = b->dims[n];
#endif
    for (n=b->ndims; n < 3; n++) local[n] = 1;

    return 0;
}



int sdf_factor(sdf_file_t *h)
{
    sdf_block_t *b = h->current_block;
    int n;
#ifdef PARALLEL
    int old_dims[6];

    // Adjust dimensions to those of a cell-centred variable
    for (n = 0; n < b->ndims; n++) {
        old_dims[n] = b->dims[n];
        if (b->stagger & n) b->dims[n]--;
        if (b->dims[n] < 1) b->dims[n] = 1;
    }

    if (b->ndims == 2)
        factor2d(h->ncpus, b->dims, b->cpu_split);
    else
        factor3d(h->ncpus, b->dims, b->cpu_split);

    // Return dimensions back to their original values
    for (n = 0; n < b->ndims; n++)
        b->dims[n] = old_dims[n];

    sdf_get_domain_extents(h, h->rank, b->starts, b->local_dims);
#else
    for (n = 0; n < 3; n++) b->local_dims[n] = b->dims[n];
#endif

    b->nlocal = 1;
    for (n = 0; n < b->ndims; n++) b->nlocal *= b->local_dims[n];

    return 0;
}



int sdf_convert_array_to_float(sdf_file_t *h, void **var_in, int count)
{
    sdf_block_t *b = h->current_block;

    if (h->use_float && b->datatype == SDF_DATATYPE_REAL8) {
        int i;
        float *r4;
        double *old_var, *r8;
        r8 = old_var = *var_in;
        r4 = *var_in = malloc(count * sizeof(float));
        for (i=0; i < count; i++)
            *r4++ = (float)(*r8++);
        if (!h->mmap) free(old_var);
        b->datatype_out = SDF_DATATYPE_REAL4;
        b->type_size_out = 4;
#ifdef PARALLEL
        b->mpitype_out = MPI_FLOAT;
#endif
    }
    return 0;
}



int sdf_randomize_array(sdf_file_t *h, void **var_in, int count)
{
    sdf_block_t *b = h->current_block;

    sdf_random_init();

    if (b->datatype_out == SDF_DATATYPE_REAL8) {
        double tmp, *array = (double*)(*var_in);
        int i, id1, id2;

        for (i=0; i < count; i++) {
            id1 = 1LL * count * sdf_random() / SDF_MAX_RND;
            id2 = 1LL * count * sdf_random() / SDF_MAX_RND;
            tmp = array[id1];
            array[id1] = array[id2];
            array[id2] = tmp;
        }
    } else {
        float tmp, *array = (float*)(*var_in);
        int i, id1, id2;

        for (i=0; i < count; i++) {
            id1 = 1LL * count * sdf_random() / SDF_MAX_RND;
            id2 = 1LL * count * sdf_random() / SDF_MAX_RND;
            tmp = array[id1];
            array[id1] = array[id2];
            array[id2] = tmp;
        }
    }

    return 0;
}


static uint32_t Q[41790], indx, carry, xcng, xs;

#define CNG (xcng = 69609 * xcng + 123)
#define XS (xs ^= xs<<13, xs ^= (unsigned)xs>>17, xs ^= xs>>5 )
#define SUPR (indx < 41790 ? Q[indx++] : refill())
#define KISS SUPR + CNG + XS

static uint32_t refill(void)
{
    int i;
    uint64_t t;
    for (i=0; i<41790; i++) {
        t = 7010176LL * Q[i] + carry;
        carry = (t>>32);
        Q[i] =~ (t);
    }
    indx = 1;
    return (Q[0]);
}


static uint32_t sdf_random(void)
{
    return KISS;
}


static void sdf_random_init(void)
{
    int i;
    indx = 41790;
    carry = 362436;
    xcng = 1236789;
    xs = 521288629;
    for (i=0; i<41790; i++) Q[i] = CNG + XS;
    for (i=0; i<41790; i++) sdf_random();
}
