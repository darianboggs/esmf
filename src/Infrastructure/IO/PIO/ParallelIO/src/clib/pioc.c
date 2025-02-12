/**
 * @file
 * Some initialization and support functions.
 * @author Jim Edwards
 * @date  2014
 *
 * @see https://github.com/NCAR/ParallelIO
 */
#include <config.h>
#include <pio.h>
#include <pio_internal.h>
#include <parallel_sort.h>

#ifdef NETCDF_INTEGRATION
#include "ncintdispatch.h"
#endif /* NETCDF_INTEGRATION */

#ifdef USE_MPE
/* The event numbers for MPE logging. */
extern int event_num[2][NUM_EVENTS];
#endif /* USE_MPE */

#ifdef NETCDF_INTEGRATION
/* Have we initialized the netcdf integration code? */
extern int ncint_initialized;

/* This is used as the default iosysid for the netcdf integration
 * code. */
extern int diosysid;
#endif /* NETCDF_INTEGRATION */

/**
 * @defgroup PIO_init_c Initialize the IO System
 * Initialize the IOSystem, including specifying number of IO and
 * computation tasks in C.
 *
 * @defgroup PIO_finalize_c Shut Down the IO System
 * Shut down an IOSystem, freeing all associated resources in C.
 *
 * @defgroup PIO_initdecomp_c Initialize a Decomposition
 * Intiailize a decomposition of data into distributed arrays in C.
 *
 * @defgroup PIO_freedecomp_c Free a Decomposition
 * Free a decomposition, and associated resources in C.
 *
 * @defgroup PIO_setframe_c Set the Record Number
 * Set the record number for a future call to PIOc_write_darray() or
 * PIOc_read_darray() in C.
 *
 * @defgroup PIO_set_hint_c Set a Hint
 * Set an MPI Hint in C.
 *
 * @defgroup PIO_error_method_c Set Error Handling
 * Set the error handling method in case error is encountered in C.
 *
 * @defgroup PIO_get_local_array_size_c Get the Local Size
 * Get the local size of a distributed array in C.
 *
 * @defgroup PIO_iosystem_is_active_c Check IOSystem
 * Is the IO system active (in C)?
 *
 * @defgroup PIO_getnumiotasks_c Get Number IO Tasks
 * Get the Number of IO Tasks in C.
 *
 * @defgroup PIO_set_blocksize_c Set Blocksize
 * Set the Blocksize in C.
 */

/** The default error handler used when iosystem cannot be located. */
int default_error_handler = PIO_INTERNAL_ERROR;

/** The target blocksize for each io task when the box rearranger is
 * used (see pio_sc.c). */
extern int blocksize;

/** Used when assiging decomposition IDs. */
int pio_next_ioid = 512;

/** Sort map. */
struct sort_map {
    int remap;
    PIO_Offset map;
};

/**
 * Check to see if PIO has been initialized.
 *
 * @param iosysid the IO system ID
 * @param active pointer that gets true if IO system is active, false
 * otherwise.
 * @returns 0 on success, error code otherwise
 * @ingroup PIO_iosystem_is_active_c
 * @author Jim Edwards
 */
int
PIOc_iosystem_is_active(int iosysid, bool *active)
{
    iosystem_desc_t *ios;

    /* Get the ios if there is one. */
    ios = pio_get_iosystem_from_id(iosysid);

    if (active)
    {
        if (!ios || (ios->comp_comm == MPI_COMM_NULL && ios->io_comm == MPI_COMM_NULL))
            *active = false;
        else
            *active = true;
    }

    return PIO_NOERR;
}

/**
 * Check to see if PIO file is open.
 *
 * @param ncid the ncid of an open file
 * @returns 1 if file is open, 0 otherwise.
 * @ingroup PIO_file_open_c
 * @author Jim Edwards
 */
int
PIOc_File_is_Open(int ncid)
{
    file_desc_t *file;

    /* If get file returns non-zero, then this file is not open. */
    if (pio_get_file(ncid, &file))
        return 0;
    else
        return 1;
}

/**
 * Set the error handling method to be used for subsequent pio library
 * calls, returns the previous method setting. Note that this changes
 * error handling for the IO system that was used when this file was
 * opened. Other files opened with the same IO system will also he
 * affected by this call. This function is supported but
 * deprecated. New code should use PIOc_set_iosystem_error_handling().
 * This method has no way to return an error, so any failure will
 * result in MPI_Abort.
 *
 * @param ncid the ncid of an open file
 * @param method the error handling method
 * @returns old error handler
 * @ingroup PIO_error_method_c
 * @author Jim Edwards
 */
int
PIOc_Set_File_Error_Handling(int ncid, int method)
{
    file_desc_t *file;
    int oldmethod;

    /* Get the file info. */
    if (pio_get_file(ncid, &file))
        piodie("Could not find file", __FILE__, __LINE__);

    /* Check that valid error handler was provided. */
    if (method != PIO_INTERNAL_ERROR && method != PIO_BCAST_ERROR &&
        method != PIO_RETURN_ERROR)
        piodie("Invalid error hanlder method", __FILE__, __LINE__);

    /* Get the old method. */
    oldmethod = file->iosystem->error_handler;

    /* Set the error hanlder. */
    file->iosystem->error_handler = method;

    return oldmethod;
}

/**
 * Increment the unlimited dimension of the given variable.
 *
 * @param ncid the ncid of the open file
 * @param varid the variable ID
 * @returns 0 on success, error code otherwise
 * @ingroup PIO_setframe_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_advanceframe(int ncid, int varid)
{
    iosystem_desc_t *ios;     /* Pointer to io system information. */
    file_desc_t *file;        /* Pointer to file information. */
    var_desc_t *vdesc;        /* Info about the var. */
    int mpierr = MPI_SUCCESS, mpierr2;  /* Return code from MPI function codes. */
    int ret;

    PLOG((1, "PIOc_advanceframe ncid = %d varid = %d", ncid, varid));

    /* Get the file info. */
    if ((ret = pio_get_file(ncid, &file)))
        return pio_err(NULL, NULL, ret, __FILE__, __LINE__);
    ios = file->iosystem;

    /* Get info about variable. */
    if ((ret = get_var_desc(varid, &file->varlist, &vdesc)))
        return pio_err(ios, file, ret, __FILE__, __LINE__);

    /* If using async, and not an IO task, then send parameters. */
    if (ios->async)
    {
        if (!ios->ioproc)
        {
            int msg = PIO_MSG_ADVANCEFRAME;

            if (ios->compmaster == MPI_ROOT)
                mpierr = MPI_Send(&msg, 1, MPI_INT, ios->ioroot, 1, ios->union_comm);

            if (!mpierr)
                mpierr = MPI_Bcast(&ncid, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&varid, 1, MPI_INT, ios->compmaster, ios->intercomm);
        }

        /* Handle MPI errors. */
        if ((mpierr2 = MPI_Bcast(&mpierr, 1, MPI_INT, ios->comproot, ios->my_comm)))
            check_mpi(ios, NULL, mpierr2, __FILE__, __LINE__);
        if (mpierr)
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
    }

    /* Increment the record number. */
    /* file->varlist[varid].record++; */
    vdesc->record++;

    return PIO_NOERR;
}

/**
 * Set the unlimited dimension of the given variable
 *
 * @param ncid the ncid of the file.
 * @param varid the varid of the variable
 * @param frame the value of the unlimited dimension.  In c 0 for the
 * first record, 1 for the second
 * @return PIO_NOERR for no error, or error code.
 * @ingroup PIO_setframe_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_setframe(int ncid, int varid, int frame)
{
    iosystem_desc_t *ios;     /* Pointer to io system information. */
    file_desc_t *file;        /* Pointer to file information. */
    var_desc_t *vdesc;        /* Info about the var. */
    int mpierr = MPI_SUCCESS, mpierr2;  /* Return code from MPI function codes. */
    int ret;

    PLOG((1, "PIOc_setframe ncid = %d varid = %d frame = %d", ncid,
          varid, frame));

    /* Get file info. */
    if ((ret = pio_get_file(ncid, &file)))
        return pio_err(NULL, NULL, ret, __FILE__, __LINE__);
    ios = file->iosystem;

    /* Get info about variable. */
    if ((ret = get_var_desc(varid, &file->varlist, &vdesc)))
        return pio_err(ios, file, ret, __FILE__, __LINE__);

    /* If using async, and not an IO task, then send parameters. */
    if (ios->async)
    {
        if (!ios->ioproc)
        {
            int msg = PIO_MSG_SETFRAME;

            if (ios->compmaster == MPI_ROOT)
                mpierr = MPI_Send(&msg, 1, MPI_INT, ios->ioroot, 1, ios->union_comm);

            if (!mpierr)
                mpierr = MPI_Bcast(&ncid, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&varid, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&frame, 1, MPI_INT, ios->compmaster, ios->intercomm);
        }

        /* Handle MPI errors. */
        if ((mpierr2 = MPI_Bcast(&mpierr, 1, MPI_INT, ios->comproot, ios->my_comm)))
            check_mpi(ios, NULL, mpierr2, __FILE__, __LINE__);
        if (mpierr)
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
    }

    /* Set the record dimension value for this variable. This will be
     * used by the write_darray functions. */
    vdesc->record = frame;

    return PIO_NOERR;
}

/**
 * Get the number of IO tasks set.
 *
 * @param iosysid the IO system ID
 * @param numiotasks a pointer taht gets the number of IO
 * tasks. Ignored if NULL.
 * @returns 0 on success, error code otherwise
 * @ingroup PIO_getnumiotasks_c
 * @author Ed Hartnett
 */
int
PIOc_get_numiotasks(int iosysid, int *numiotasks)
{
    iosystem_desc_t *ios;

    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    if (numiotasks)
        *numiotasks = ios->num_iotasks;

    return PIO_NOERR;
}

/**
 * Get the local size of the variable.
 *
 * @param ioid IO descrption ID.
 * @returns the size of the array.
 * @ingroup PIO_get_local_array_size_c
 * @author Jim Edwards
 */
int
PIOc_get_local_array_size(int ioid)
{
    io_desc_t *iodesc;

    if (!(iodesc = pio_get_iodesc_from_id(ioid)))
        piodie("Could not get iodesc", __FILE__, __LINE__);

    return iodesc->ndof;
}

/**
 * Set the error handling method used for subsequent calls. This
 * function is deprecated. New code should use
 * PIOc_set_iosystem_error_handling(). This method has no way to
 * return an error, so any failure will result in MPI_Abort.
 *
 * @param iosysid the IO system ID
 * @param method the error handling method
 * @returns old error handler
 * @ingroup PIO_error_method_c
 * @author Jim Edwards
 */
int
PIOc_Set_IOSystem_Error_Handling(int iosysid, int method)
{
    iosystem_desc_t *ios;
    int oldmethod;

    /* Get the iosystem info. */
    if (iosysid != PIO_DEFAULT)
        if (!(ios = pio_get_iosystem_from_id(iosysid)))
            piodie("Could not find IO system.", __FILE__, __LINE__);

    /* Set the error handler. */
    if (PIOc_set_iosystem_error_handling(iosysid, method, &oldmethod))
        piodie("Could not set the IOSystem error hanlder", __FILE__, __LINE__);

    return oldmethod;
}

/**
 * Set the error handling method used for subsequent calls for this IO
 * system.
 *
 * @param iosysid the IO system ID. Passing PIO_DEFAULT instead
 * changes the default error handling for the library.
 * @param method the error handling method
 * @param old_method pointer to int that will get old method. Ignored
 * if NULL.
 * @returns 0 for success, error code otherwise.
 * @ingroup PIO_error_method_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_set_iosystem_error_handling(int iosysid, int method, int *old_method)
{
    iosystem_desc_t *ios = NULL;
    int mpierr = MPI_SUCCESS, mpierr2;  /* Return code from MPI function codes. */

    PLOG((1, "PIOc_set_iosystem_error_handling iosysid = %d method = %d", iosysid,
          method));

    /* Find info about this iosystem. */
    if (iosysid != PIO_DEFAULT)
        if (!(ios = pio_get_iosystem_from_id(iosysid)))
            return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    /* Check that valid error handler was provided. */
    if (method != PIO_INTERNAL_ERROR && method != PIO_BCAST_ERROR &&
        method != PIO_RETURN_ERROR)
        return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    /* If using async, and not an IO task, then send parameters. */
    if (iosysid != PIO_DEFAULT)
        if (ios->async)
        {
            if (!ios->ioproc)
            {
                int msg = PIO_MSG_SETERRORHANDLING;
                char old_method_present = old_method ? true : false;

                if (ios->compmaster == MPI_ROOT)
                    mpierr = MPI_Send(&msg, 1, MPI_INT, ios->ioroot, 1, ios->union_comm);

                if (!mpierr)
                    mpierr = MPI_Bcast(&method, 1, MPI_INT, ios->compmaster, ios->intercomm);
                if (!mpierr)
                    mpierr = MPI_Bcast(&old_method_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            }

            /* Handle MPI errors. */
            if ((mpierr2 = MPI_Bcast(&mpierr, 1, MPI_INT, ios->comproot, ios->my_comm)))
                check_mpi(ios, NULL, mpierr2, __FILE__, __LINE__);
            if (mpierr)
                return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
        }

    /* Return the current handler. */
    if (old_method)
        *old_method = iosysid == PIO_DEFAULT ? default_error_handler : ios->error_handler;

    /* Set new error handler. */
    if (iosysid == PIO_DEFAULT)
        default_error_handler = method;
    else
        ios->error_handler = method;

    return PIO_NOERR;
}

/**
 * Compare.
 *
 * @param a pointer to a
 * @param b pointer to b
 * @return -1 if a.map < b.map, 1 if a.map > b.map, 0 if equal
 * @author Jim Edwards
 */
int
compare( const void* a, const void* b)
{
    struct sort_map l_a = * ( (struct sort_map *) a );
    struct sort_map l_b = * ( (struct sort_map *) b );

    if ( l_a.map < l_b.map )
        return -1;
    else if ( l_a.map > l_b.map )
        return 1;
    return 0;
}

/**
 * Initialize the decomposition used with distributed arrays. The
 * decomposition describes how the data will be distributed between
 * tasks.
 *
 * Internally, this function will:
 * <ul>
 * <li>Allocate and initialize an iodesc struct for this
 * decomposition. (This also allocates an io_region struct for the
 * first region.)
 * <li>(Box rearranger only) If iostart or iocount are NULL, call
 * CalcStartandCount() to determine starts/counts. Then call
 * compute_maxIObuffersize() to compute the max IO buffer size needed.
 * <li>Create the rearranger.
 * <li>Assign an ioid and add this decomposition to the list of open
 * decompositions.
 * </ul>
 *
 * @param iosysid the IO system ID.
 * @param pio_type the basic PIO data type used.
 * @param ndims the number of dimensions in the variable, not
 * including the unlimited dimension.
 * @param gdimlen an array length ndims with the sizes of the global
 * dimensions.
 * @param maplen the local length of the compmap array.
 * @param compmap a 1 based array of offsets into the array record on
 * file. A 0 in this array indicates a value which should not be
 * transfered.
 * @param ioidp pointer that will get the io description ID. Ignored
 * if NULL.
 * @param rearranger pointer to the rearranger to be used for this
 * decomp or NULL to use the default.
 * @param iostart An array of start values for block cyclic
 * decompositions for the SUBSET rearranger. Ignored if block
 * rearranger is used. If NULL and SUBSET rearranger is used, the
 * iostarts are generated.
 * @param iocount An array of count values for block cyclic
 * decompositions for the SUBSET rearranger. Ignored if block
 * rearranger is used. If NULL and SUBSET rearranger is used, the
 * iostarts are generated.
 * @returns 0 on success, error code otherwise
 * @ingroup PIO_initdecomp_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_InitDecomp(int iosysid, int pio_type, int ndims, const int *gdimlen, int maplen,
                const PIO_Offset *compmap, int *ioidp, const int *rearranger,
                const PIO_Offset *iostart, const PIO_Offset *iocount)
{
    iosystem_desc_t *ios;  /* Pointer to io system information. */
    io_desc_t *iodesc;     /* The IO description. */
    int mpierr = MPI_SUCCESS, mpierr2;  /* Return code from MPI function calls. */
    int ierr;              /* Return code. */

    PLOG((1, "PIOc_InitDecomp iosysid = %d pio_type = %d ndims = %d maplen = %d",
          iosysid, pio_type, ndims, maplen));

#ifdef USE_MPE
    pio_start_mpe_log(DECOMP);
#endif /* USE_MPE */

    /* Get IO system info. */
    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    /* Caller must provide these. */
    if (!gdimlen || !compmap || !ioidp)
        return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    /* Check the dim lengths. */
    for (int i = 0; i < ndims; i++)
        if (gdimlen[i] <= 0)
            return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    /* If async is in use, and this is not an IO task, bcast the parameters. */
    if (ios->async)
    {
        if (!ios->ioproc)
        {
            int msg = PIO_MSG_INITDECOMP_DOF; /* Message for async notification. */
            char rearranger_present = rearranger ? true : false;
            char iostart_present = iostart ? true : false;
            char iocount_present = iocount ? true : false;
            if (ios->compmaster == MPI_ROOT){
                PLOG((1, "about to sent msg %d union_comm %d",msg,ios->union_comm));
                mpierr = MPI_Send(&msg, 1, MPI_INT, ios->ioroot, 1, ios->union_comm);
            }
            if (!mpierr)
                mpierr = MPI_Bcast(&iosysid, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&pio_type, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&ndims, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast((int *)gdimlen, ndims, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&maplen, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast((PIO_Offset *)compmap, maplen, MPI_OFFSET, ios->compmaster, ios->intercomm);

            if (!mpierr)
                mpierr = MPI_Bcast(&rearranger_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (rearranger_present && !mpierr)
                mpierr = MPI_Bcast((int *)rearranger, 1, MPI_INT, ios->compmaster, ios->intercomm);

            if (!mpierr)
                mpierr = MPI_Bcast(&iostart_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (iostart_present && !mpierr)
                mpierr = MPI_Bcast((PIO_Offset *)iostart, ndims, MPI_OFFSET, ios->compmaster, ios->intercomm);

            if (!mpierr)
                mpierr = MPI_Bcast(&iocount_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (iocount_present && !mpierr)
                mpierr = MPI_Bcast((PIO_Offset *)iocount, ndims, MPI_OFFSET, ios->compmaster, ios->intercomm);
            PLOG((2, "PIOc_InitDecomp iosysid = %d pio_type = %d ndims = %d maplen = %d rearranger_present = %d iostart_present = %d "
                  "iocount_present = %d ", iosysid, pio_type, ndims, maplen, rearranger_present, iostart_present, iocount_present));
        }

        /* Handle MPI errors. */
        if ((mpierr2 = MPI_Bcast(&mpierr, 1, MPI_INT, ios->comproot, ios->my_comm)))
            return check_mpi(ios, NULL, mpierr2, __FILE__, __LINE__);
        if (mpierr)
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

        if(rearranger && (*rearranger != ios->default_rearranger))
            return pio_err(ios, NULL, PIO_EBADREARR, __FILE__,__LINE__);

    }

    /* Allocate space for the iodesc info. This also allocates the
     * first region and copies the rearranger opts into this
     * iodesc. */
    PLOG((2, "allocating iodesc pio_type %d ndims %d", pio_type, ndims));
    if ((ierr = malloc_iodesc(ios, pio_type, ndims, &iodesc)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);

    /* Remember the maplen. */
    iodesc->maplen = maplen;

    /* Remember the map. */
    if (!(iodesc->map = malloc(sizeof(PIO_Offset) * maplen)))
        return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
    iodesc->needssort = false;
    iodesc->remap = NULL;
    for (int m = 0; m < maplen; m++)
    {
        if(m > 0 && compmap[m] > 0 && compmap[m] < compmap[m-1])
        {
            iodesc->needssort = true;
            PLOG((2, "compmap[%d] = %ld compmap[%d]= %ld", m, compmap[m], m-1, compmap[m-1]));
            break;
        }
    }
    if (iodesc->needssort)
    {
        struct sort_map *tmpsort;

        if (!(tmpsort = malloc(sizeof(struct sort_map) * maplen)))
            return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
        if (!(iodesc->remap = malloc(sizeof(int) * maplen)))
        {
            free(tmpsort);
            return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
        }
        for (int m=0; m < maplen; m++)
        {
            tmpsort[m].remap = m;
            tmpsort[m].map = compmap[m];
        }
        qsort( tmpsort, maplen, sizeof(struct sort_map), compare );
        for (int m=0; m < maplen; m++)
        {
            iodesc->map[m] = compmap[tmpsort[m].remap];
            iodesc->remap[m] = tmpsort[m].remap;
        }
        free(tmpsort);
    }
    else
    {
        for (int m=0; m < maplen; m++)
        {
            iodesc->map[m] = compmap[m];
        }
    }

    /* Remember the dim sizes. */
    if (!(iodesc->dimlen = malloc(sizeof(int) * ndims)))
        return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
    for (int d = 0; d < ndims; d++)
        iodesc->dimlen[d] = gdimlen[d];

    /* Set the rearranger. */
    if (!rearranger)
        iodesc->rearranger = ios->default_rearranger;
    else
        iodesc->rearranger = *rearranger;
    PLOG((2, "iodesc->rearranger = %d", iodesc->rearranger));

    /* Is this the subset rearranger? */
    if (iodesc->rearranger == PIO_REARR_SUBSET)
    {
      /* check if the decomp is valid for write or is read-only */
        iodesc->readonly = false;
/*      if(ios->compproc){
        // It should be okay to use compmap here but test_darray_fill shows
        // the compmap array modified by this call, TODO - investigate this.
        PIO_Offset *tmpmap;
        if (!(tmpmap = malloc(sizeof(PIO_Offset) * maplen)))
          return PIO_ENOMEM;
        memcpy(tmpmap, compmap, maplen*sizeof(PIO_Offset));
        if((ierr = run_unique_check(ios->comp_comm, (size_t) maplen, tmpmap, &iodesc->readonly)))
            return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
        free(tmpmap);
      }
*/
        /*      printf("readonly: %d\n",iodesc->readonly);
        for(int i=0;i<maplen;i++)
        printf("compmap[%d]=%d\n",i,compmap[i]); */
        iodesc->num_aiotasks = ios->num_iotasks;
        PLOG((2, "creating subset rearranger iodesc->num_aiotasks = %d readonly = %d",
              iodesc->num_aiotasks, iodesc->readonly));
        if ((ierr = subset_rearrange_create(ios, maplen, (PIO_Offset *)iodesc->map, gdimlen,
                                            ndims, iodesc)))
            return pio_err(ios, NULL, ierr, __FILE__, __LINE__);

    }
    else /* box rearranger */
    {
        if (ios->ioproc)
        {
            /*  Unless the user specifies the start and count for each
             *  IO task compute it. */
            if (iostart && iocount)
            {
                PLOG((3, "iostart and iocount provided"));
                for (int i = 0; i < ndims; i++)
                {
                    iodesc->firstregion->start[i] = iostart[i];
                    iodesc->firstregion->count[i] = iocount[i];
                }
                iodesc->num_aiotasks = ios->num_iotasks;
            }
            else
            {
                /* Compute start and count values for each io task. */
                PLOG((2, "about to call CalcStartandCount pio_type = %d ndims = %d", pio_type, ndims));
                if ((ierr = CalcStartandCount(pio_type, ndims, gdimlen, ios->num_iotasks,
                                              ios->io_rank, iodesc->firstregion->start,
                                              iodesc->firstregion->count, &iodesc->num_aiotasks)))
                    return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
            }

            /* Compute the max io buffer size needed for an iodesc. */
            if ((ierr = compute_maxIObuffersize(ios->io_comm, iodesc)))
                return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
            PLOG((3, "compute_maxIObuffersize called iodesc->maxiobuflen = %d",
                  iodesc->maxiobuflen));
        }

        /* Depending on array size and io-blocksize the actual number
         * of io tasks used may vary. */
        if ((mpierr = MPI_Bcast(&(iodesc->num_aiotasks), 1, MPI_INT, ios->ioroot,
                                ios->my_comm)))
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
        PLOG((3, "iodesc->num_aiotasks = %d", iodesc->num_aiotasks));

        /* Compute the communications pattern for this decomposition. */
        if (iodesc->rearranger == PIO_REARR_BOX)
            if ((ierr = box_rearrange_create(ios, maplen, iodesc->map, gdimlen, ndims, iodesc)))
                return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
    }

    /* Broadcast next ioid to all tasks from io root.*/
    if (ios->async)
    {
        PLOG((3, "initdecomp bcasting pio_next_ioid %d", pio_next_ioid));
        if ((mpierr = MPI_Bcast(&pio_next_ioid, 1, MPI_INT, ios->ioroot, ios->my_comm)))
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
        PLOG((3, "initdecomp bcast pio_next_ioid %d", pio_next_ioid));
    }

    /* Set the decomposition ID. */
    iodesc->ioid = pio_next_ioid++;
    if (ioidp)
        *ioidp = iodesc->ioid;

    /* Add this IO description to the list. */
    if ((ierr = pio_add_to_iodesc_list(iodesc)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);

#if PIO_ENABLE_LOGGING
    /* Log results. */
    PLOG((2, "iodesc ioid = %d nrecvs = %d ndof = %d ndims = %d num_aiotasks = %d "
          "rearranger = %d maxregions = %d needsfill = %d llen = %d maxiobuflen  = %d",
          iodesc->ioid, iodesc->nrecvs, iodesc->ndof, iodesc->ndims, iodesc->num_aiotasks,
          iodesc->rearranger, iodesc->maxregions, iodesc->needsfill, iodesc->llen,
          iodesc->maxiobuflen));
    if (iodesc->rindex)
        for (int j = 0; j < iodesc->llen; j++)
            PLOG((3, "rindex[%d] = %lld", j, iodesc->rindex[j]));
#endif /* PIO_ENABLE_LOGGING */

    /* This function only does something if pre-processor macro
     * PERFTUNE is set. */
    performance_tune_rearranger(ios, iodesc);

#ifdef USE_MPE
    pio_stop_mpe_log(DECOMP, __func__);
#endif /* USE_MPE */

    return PIO_NOERR;
}

/**
 * Initialize the decomposition used with distributed arrays. The
 * decomposition describes how the data will be distributed between
 * tasks.  This is a readonly version of this function.  In this version
 * compmap is allowed to have repeated mappings so that one value on storage
 * may be read to multiple locations in memory.
 *
 * Internally, this function will:
 * <ul>
 * <li>Allocate and initialize an iodesc struct for this
 * decomposition. (This also allocates an io_region struct for the
 * first region.)
 * <li>(Box rearranger only) If iostart or iocount are NULL, call
 * CalcStartandCount() to determine starts/counts. Then call
 * compute_maxIObuffersize() to compute the max IO buffer size needed.
 * <li>Create the rearranger.
 * <li>Assign an ioid and add this decomposition to the list of open
 * decompositions.
 * </ul>
 *
 * @param iosysid the IO system ID.
 * @param pio_type the basic PIO data type used.
 * @param ndims the number of dimensions in the variable, not
 * including the unlimited dimension.
 * @param gdimlen an array length ndims with the sizes of the global
 * dimensions.
 * @param maplen the local length of the compmap array.
 * @param compmap a 1 based array of offsets into the array record on
 * file. A 0 in this array indicates a value which should not be
 * transfered.
 * @param ioidp pointer that will get the io description ID. Ignored
 * if NULL.
 * @param rearranger pointer to the rearranger to be used for this
 * decomp or NULL to use the default.
 * @param iostart An array of start values for block cyclic
 * decompositions for the SUBSET rearranger. Ignored if block
 * rearranger is used. If NULL and SUBSET rearranger is used, the
 * iostarts are generated.
 * @param iocount An array of count values for block cyclic
 * decompositions for the SUBSET rearranger. Ignored if block
 * rearranger is used. If NULL and SUBSET rearranger is used, the
 * iostarts are generated.
 * @returns 0 on success, error code otherwise
 * @ingroup PIO_initdecomp_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_InitDecomp_ReadOnly(int iosysid, int pio_type, int ndims, const int *gdimlen, int maplen,
                const PIO_Offset *compmap, int *ioidp, const int *rearranger,
                const PIO_Offset *iostart, const PIO_Offset *iocount)
{
    iosystem_desc_t *ios;  /* Pointer to io system information. */
    io_desc_t *iodesc;     /* The IO description. */
    int mpierr = MPI_SUCCESS, mpierr2;  /* Return code from MPI function calls. */
    int ierr;              /* Return code. */

    PLOG((1, "PIOc_InitDecomp iosysid = %d pio_type = %d ndims = %d maplen = %d",
          iosysid, pio_type, ndims, maplen));

#ifdef USE_MPE
    pio_start_mpe_log(DECOMP);
#endif /* USE_MPE */

    /* Get IO system info. */
    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    /* Caller must provide these. */
    if (!gdimlen || !compmap || !ioidp)
        return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    /* Check the dim lengths. */
    for (int i = 0; i < ndims; i++)
        if (gdimlen[i] <= 0)
            return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    /* If async is in use, and this is not an IO task, bcast the parameters. */
    if (ios->async)
    {
        if (!ios->ioproc)
        {
            int msg = PIO_MSG_INITDECOMP_DOF; /* Message for async notification. */
            char rearranger_present = rearranger ? true : false;
            char iostart_present = iostart ? true : false;
            char iocount_present = iocount ? true : false;
            if (ios->compmaster == MPI_ROOT){
                PLOG((1, "about to sent msg %d union_comm %d",msg,ios->union_comm));
                mpierr = MPI_Send(&msg, 1, MPI_INT, ios->ioroot, 1, ios->union_comm);
            }
            if (!mpierr)
                mpierr = MPI_Bcast(&iosysid, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&pio_type, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&ndims, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast((int *)gdimlen, ndims, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast(&maplen, 1, MPI_INT, ios->compmaster, ios->intercomm);
            if (!mpierr)
                mpierr = MPI_Bcast((PIO_Offset *)compmap, maplen, MPI_OFFSET, ios->compmaster, ios->intercomm);

            if (!mpierr)
                mpierr = MPI_Bcast(&rearranger_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (rearranger_present && !mpierr)
                mpierr = MPI_Bcast((int *)rearranger, 1, MPI_INT, ios->compmaster, ios->intercomm);

            if (!mpierr)
                mpierr = MPI_Bcast(&iostart_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (iostart_present && !mpierr)
                mpierr = MPI_Bcast((PIO_Offset *)iostart, ndims, MPI_OFFSET, ios->compmaster, ios->intercomm);

            if (!mpierr)
                mpierr = MPI_Bcast(&iocount_present, 1, MPI_CHAR, ios->compmaster, ios->intercomm);
            if (iocount_present && !mpierr)
                mpierr = MPI_Bcast((PIO_Offset *)iocount, ndims, MPI_OFFSET, ios->compmaster, ios->intercomm);
            PLOG((2, "PIOc_InitDecomp iosysid = %d pio_type = %d ndims = %d maplen = %d rearranger_present = %d iostart_present = %d "
                  "iocount_present = %d ", iosysid, pio_type, ndims, maplen, rearranger_present, iostart_present, iocount_present));
        }

        /* Handle MPI errors. */
        if ((mpierr2 = MPI_Bcast(&mpierr, 1, MPI_INT, ios->comproot, ios->my_comm)))
            return check_mpi(ios, NULL, mpierr2, __FILE__, __LINE__);
        if (mpierr)
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

        if(rearranger && (*rearranger != ios->default_rearranger))
            return pio_err(ios, NULL, PIO_EBADREARR, __FILE__,__LINE__);

    }

    /* Allocate space for the iodesc info. This also allocates the
     * first region and copies the rearranger opts into this
     * iodesc. */
    PLOG((2, "allocating iodesc pio_type %d ndims %d", pio_type, ndims));
    if ((ierr = malloc_iodesc(ios, pio_type, ndims, &iodesc)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);

    /* Remember the maplen. */
    iodesc->maplen = maplen;

    /* Remember the map. */
    if (!(iodesc->map = malloc(sizeof(PIO_Offset) * maplen)))
        return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
    iodesc->needssort = false;
    iodesc->remap = NULL;
    for (int m = 0; m < maplen; m++)
    {
        if(m > 0 && compmap[m] > 0 && compmap[m] < compmap[m-1])
        {
            iodesc->needssort = true;
            PLOG((2, "compmap[%d] = %ld compmap[%d]= %ld", m, compmap[m], m-1, compmap[m-1]));
            break;
        }
    }
    if (iodesc->needssort)
    {
        struct sort_map *tmpsort;

        if (!(tmpsort = malloc(sizeof(struct sort_map) * maplen)))
            return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
        if (!(iodesc->remap = malloc(sizeof(int) * maplen)))
        {
            free(tmpsort);
            return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
        }
        for (int m=0; m < maplen; m++)
        {
            tmpsort[m].remap = m;
            tmpsort[m].map = compmap[m];
        }
        qsort( tmpsort, maplen, sizeof(struct sort_map), compare );
        for (int m=0; m < maplen; m++)
        {
            iodesc->map[m] = compmap[tmpsort[m].remap];
            iodesc->remap[m] = tmpsort[m].remap;
        }
        free(tmpsort);
    }
    else
    {
        for (int m=0; m < maplen; m++)
        {
            iodesc->map[m] = compmap[m];
        }
    }

    /* Remember the dim sizes. */
    if (!(iodesc->dimlen = malloc(sizeof(int) * ndims)))
        return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
    for (int d = 0; d < ndims; d++)
        iodesc->dimlen[d] = gdimlen[d];

    /* Set the rearranger. */
    if (!rearranger)
        iodesc->rearranger = ios->default_rearranger;
    else
        iodesc->rearranger = *rearranger;

    if (iodesc->rearranger != PIO_REARR_SUBSET){

        return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);
    }

    PLOG((2, "iodesc->rearranger = %d", iodesc->rearranger));

    /* Is this the subset rearranger? */
    if (iodesc->rearranger == PIO_REARR_SUBSET)
    {
        iodesc->readonly = true;
      /* check if the decomp is valid for write or is read-only */
/*      if(ios->compproc){
        // It should be okay to use compmap here but test_darray_fill shows
        // the compmap array modified by this call, TODO - investigate this.
        PIO_Offset *tmpmap;
        if (!(tmpmap = malloc(sizeof(PIO_Offset) * maplen)))
          return PIO_ENOMEM;
        memcpy(tmpmap, compmap, maplen*sizeof(PIO_Offset));
        if((ierr = run_unique_check(ios->comp_comm, (size_t) maplen, tmpmap, &iodesc->readonly)))
            return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
        free(tmpmap);
      }
*/        /*      printf("readonly: %d\n",iodesc->readonly);
        for(int i=0;i<maplen;i++)
        printf("compmap[%d]=%d\n",i,compmap[i]); */
        iodesc->num_aiotasks = ios->num_iotasks;
        PLOG((2, "creating subset rearranger iodesc->num_aiotasks = %d readonly = %d",
              iodesc->num_aiotasks, iodesc->readonly));
        if ((ierr = subset_rearrange_create(ios, maplen, (PIO_Offset *)iodesc->map, gdimlen,
                                            ndims, iodesc)))
            return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
    }
    else /* box rearranger */
    {
        if (ios->ioproc)
        {
            /*  Unless the user specifies the start and count for each
             *  IO task compute it. */
            if (iostart && iocount)
            {
                PLOG((3, "iostart and iocount provided"));
                for (int i = 0; i < ndims; i++)
                {
                    iodesc->firstregion->start[i] = iostart[i];
                    iodesc->firstregion->count[i] = iocount[i];
                }
                iodesc->num_aiotasks = ios->num_iotasks;
            }
            else
            {
                /* Compute start and count values for each io task. */
                PLOG((2, "about to call CalcStartandCount pio_type = %d ndims = %d", pio_type, ndims));
                if ((ierr = CalcStartandCount(pio_type, ndims, gdimlen, ios->num_iotasks,
                                              ios->io_rank, iodesc->firstregion->start,
                                              iodesc->firstregion->count, &iodesc->num_aiotasks)))
                    return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
            }

            /* Compute the max io buffer size needed for an iodesc. */
            if ((ierr = compute_maxIObuffersize(ios->io_comm, iodesc)))
                return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
            PLOG((3, "compute_maxIObuffersize called iodesc->maxiobuflen = %d",
                  iodesc->maxiobuflen));
        }

        /* Depending on array size and io-blocksize the actual number
         * of io tasks used may vary. */
        if ((mpierr = MPI_Bcast(&(iodesc->num_aiotasks), 1, MPI_INT, ios->ioroot,
                                ios->my_comm)))
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
        PLOG((3, "iodesc->num_aiotasks = %d", iodesc->num_aiotasks));

        /* Compute the communications pattern for this decomposition. */
        if (iodesc->rearranger == PIO_REARR_BOX)
            if ((ierr = box_rearrange_create(ios, maplen, iodesc->map, gdimlen, ndims, iodesc)))
                return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
    }

    /* Broadcast next ioid to all tasks from io root.*/
    if (ios->async)
    {
        PLOG((3, "initdecomp bcasting pio_next_ioid %d", pio_next_ioid));
        if ((mpierr = MPI_Bcast(&pio_next_ioid, 1, MPI_INT, ios->ioroot, ios->my_comm)))
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
        PLOG((3, "initdecomp bcast pio_next_ioid %d", pio_next_ioid));
    }

    /* Set the decomposition ID. */
    iodesc->ioid = pio_next_ioid++;
    if (ioidp)
        *ioidp = iodesc->ioid;

    /* Add this IO description to the list. */
    if ((ierr = pio_add_to_iodesc_list(iodesc)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);

#if PIO_ENABLE_LOGGING
    /* Log results. */
    PLOG((2, "iodesc ioid = %d nrecvs = %d ndof = %d ndims = %d num_aiotasks = %d "
          "rearranger = %d maxregions = %d needsfill = %d llen = %d maxiobuflen  = %d",
          iodesc->ioid, iodesc->nrecvs, iodesc->ndof, iodesc->ndims, iodesc->num_aiotasks,
          iodesc->rearranger, iodesc->maxregions, iodesc->needsfill, iodesc->llen,
          iodesc->maxiobuflen));
    if (iodesc->rindex)
        for (int j = 0; j < iodesc->llen; j++)
            PLOG((3, "rindex[%d] = %lld", j, iodesc->rindex[j]));
#endif /* PIO_ENABLE_LOGGING */

    /* This function only does something if pre-processor macro
     * PERFTUNE is set. */
    performance_tune_rearranger(ios, iodesc);

#ifdef USE_MPE
    pio_stop_mpe_log(DECOMP, __func__);
#endif /* USE_MPE */

    return PIO_NOERR;
}

/**
 * Initialize the decomposition used with distributed arrays. The
 * decomposition describes how the data will be distributed between
 * tasks.
 *
 * @param iosysid the IO system ID.
 * @param pio_type the basic PIO data type used.
 * @param ndims the number of dimensions in the variable, not
 * including the unlimited dimension.
 * @param gdimlen an array length ndims with the sizes of the global
 * dimensions.
 * @param maplen the local length of the compmap array.
 * @param compmap a 0 based array of offsets into the array record on
 * file. A -1 in this array indicates a value which should not be
 * transfered.
 * @param ioidp pointer that will get the io description ID.
 * @param rearranger the rearranger to be used for this decomp or 0 to
 * use the default. Valid rearrangers are PIO_REARR_BOX and
 * PIO_REARR_SUBSET.
 * @param iostart An array of start values for block cyclic
 * decompositions. If NULL ???
 * @param iocount An array of count values for block cyclic
 * decompositions. If NULL ???
 * @returns 0 on success, error code otherwise
 * @ingroup PIO_initdecomp_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_init_decomp(int iosysid, int pio_type, int ndims, const int *gdimlen, int maplen,
                 const PIO_Offset *compmap, int *ioidp, int rearranger,
                 const PIO_Offset *iostart, const PIO_Offset *iocount)
{
    PIO_Offset *compmap_1_based;
    int *rearrangerp = NULL;
    int ret;

    PLOG((1, "PIOc_init_decomp iosysid = %d pio_type = %d ndims = %d maplen = %d",
          iosysid, pio_type, ndims, maplen));

    /* If the user specified a non-default rearranger, use it. */
    if (rearranger)
        rearrangerp = &rearranger;

    /* Allocate storage for compmap that's one-based. */
    if (!(compmap_1_based = malloc(sizeof(PIO_Offset) * maplen)))
        return PIO_ENOMEM;

    /* Add 1 to all elements in compmap. */
    for (int e = 0; e < maplen; e++)
    {
        PLOG((3, "zero-based compmap[%d] = %d", e, compmap[e]));
        compmap_1_based[e] = compmap[e] + 1;
    }

    /* Call the legacy version of the function. */
    ret = PIOc_InitDecomp(iosysid, pio_type, ndims, gdimlen, maplen, compmap_1_based,
                          ioidp, rearrangerp, iostart, iocount);

    free(compmap_1_based);

    return ret;
}

/**
 * This is a simplified initdecomp which can be used if the memory
 * order of the data can be expressed in terms of start and count on
 * the file. In this case we compute the compdof.
 *
 * @param iosysid the IO system ID
 * @param pio_type
 * @param ndims the number of dimensions
 * @param gdimlen an array length ndims with the sizes of the global
 * dimensions.
 * @param start start array
 * @param count count array
 * @param ioidp pointer that gets the IO ID.
 * @returns 0 for success, error code otherwise
 * @ingroup PIO_initdecomp_c
 * @author Jim Edwards
 */
int
PIOc_InitDecomp_bc(int iosysid, int pio_type, int ndims, const int *gdimlen,
                   const long int *start, const long int *count, int *ioidp)

{
    iosystem_desc_t *ios;
    int n, i, maplen = 1;
    PIO_Offset prod[ndims], loc[ndims];
    int rearr = PIO_REARR_SUBSET;

    PLOG((1, "PIOc_InitDecomp_bc iosysid = %d pio_type = %d ndims = %d"));

    /* Get the info about the io system. */
    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    /* Check for required inputs. */
    if (!gdimlen || !start || !count || !ioidp)
        return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    /* Check that dim, start, and count values are not obviously
     * incorrect. */
    for (int i = 0; i < ndims; i++)
        if (gdimlen[i] <= 0 || start[i] < 0 || count[i] < 0 || (start[i] + count[i]) > gdimlen[i])
            return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    /* Find the maplen. */
    for (i = 0; i < ndims; i++)
        maplen *= count[i];

    /* Get storage for the compmap. */
    PIO_Offset compmap[maplen];

    /* Find the compmap. */
    prod[ndims - 1] = 1;
    loc[ndims - 1] = 0;
    for (n = ndims - 2; n >= 0; n--)
    {
        prod[n] = prod[n + 1] * gdimlen[n + 1];
        loc[n] = 0;
    }
    for (i = 0; i < maplen; i++)
    {
        compmap[i] = 1;
        for (n = ndims - 1; n >= 0; n--)
            compmap[i] += (start[n] + loc[n]) * prod[n];

        n = ndims - 1;
        loc[n] = (loc[n] + 1) % count[n];
        while (loc[n] == 0 && n > 0)
        {
            n--;
            loc[n] = (loc[n] + 1) % count[n];
        }
    }

    return PIOc_InitDecomp(iosysid, pio_type, ndims, gdimlen, maplen, compmap, ioidp,
                           &rearr, NULL, NULL);
}

/**
 * Library initialization used when IO tasks are a subset of compute
 * tasks.
 *
 * This function creates an MPI intracommunicator between a set of IO
 * tasks and one or more sets of computational tasks.
 *
 * The caller must create all comp_comm and the io_comm MPI
 * communicators before calling this function.
 *
 * Internally, this function does the following:
 *
 * <ul>
 * <li>Initialize logging system (if PIO_ENABLE_LOGGING is set).
 * <li>Allocates and initializes the iosystem_desc_t struct (ios).
 * <li>MPI duplicated user comp_comm to ios->comp_comm and
 * ios->union_comm.
 * <li>Set ios->my_comm to be ios->comp_comm. (Not an MPI
 * duplication.)
 * <li>Find MPI rank in comp_comm, determine ranks of IO tasks,
 * determine whether this task is one of the IO tasks.
 * <li>Identify the root IO tasks.
 * <li>Create MPI groups for IO tasks, and for computation tasks.
 * <li>On IO tasks, create an IO communicator (ios->io_comm).
 * <li>Assign an iosystemid, and put this iosystem_desc_t into the
 * list of open iosystems.
 * </ul>
 *
 * When complete, there are three MPI communicators (ios->comp_comm,
 * ios->union_comm, and ios->io_comm) that must be freed by MPI.
 *
 * @param comp_comm the MPI_Comm of the compute tasks.
 * @param num_iotasks the number of io tasks to use.
 * @param stride the offset between io tasks in the comp_comm. The mod
 * operator is used when computing the IO tasks with the formula:
 * <pre>ios->ioranks[i] = (base + i * ustride) % ios->num_comptasks</pre>.
 * @param base the comp_comm index of the first io task.
 * @param rearr the rearranger to use by default, this may be
 * overriden in the PIO_init_decomp(). The rearranger is not used
 * until the decomposition is initialized.
 * @param iosysidp index of the defined system descriptor.
 * @return 0 on success, otherwise a PIO error code.
 * @ingroup PIO_init_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_Init_Intracomm(MPI_Comm comp_comm, int num_iotasks, int stride, int base,
                    int rearr, int *iosysidp)
{
    iosystem_desc_t *ios;
    int ustride;
    MPI_Group compgroup;  /* Contains tasks involved in computation. */
    MPI_Group iogroup;    /* Contains the processors involved in I/O. */
    int num_comptasks; /* The size of the comp_comm. */
    int mpierr;        /* Return value for MPI calls. */
    int ret;           /* Return code for function calls. */

    /* Turn on the logging system. */
    if ((ret = pio_init_logging()))
        return pio_err(NULL, NULL, ret, __FILE__, __LINE__);

#ifdef NETCDF_INTEGRATION
    PLOG((1, "Initializing netcdf integration"));
    /* Initialize netCDF integration layer if we need to. */
    if (!ncint_initialized)
        PIO_NCINT_initialize();
#endif /* NETCDF_INTEGRATION */

#ifdef USE_MPE
    pio_start_mpe_log(INIT);
#endif /* USE_MPE */

    /* Find the number of computation tasks. */
    if ((mpierr = MPI_Comm_size(comp_comm, &num_comptasks)))
        return check_mpi(NULL, NULL, mpierr, __FILE__, __LINE__);

    PLOG((1, "PIOc_Init_Intracomm comp_comm = %d num_iotasks = %d stride = %d base = %d "
          "rearr = %d", comp_comm, num_iotasks, stride, base, rearr));

    /* Check the inputs. */
    if (!iosysidp || num_iotasks < 1 || num_iotasks * stride > num_comptasks)
        return pio_err(NULL, NULL, PIO_EINVAL, __FILE__, __LINE__);


    /* Allocate memory for the iosystem info. */
    if (!(ios = calloc(1, sizeof(iosystem_desc_t))))
        return pio_err(NULL, NULL, PIO_ENOMEM, __FILE__, __LINE__);

    ios->io_comm = MPI_COMM_NULL;
    ios->intercomm = MPI_COMM_NULL;
    ios->error_handler = default_error_handler;
    ios->default_rearranger = rearr;
    ios->num_iotasks = num_iotasks;
    ios->num_comptasks = num_comptasks;

    /* For non-async, the IO tasks are a subset of the comptasks. */
    ios->num_uniontasks = num_comptasks;

    /* Initialize the rearranger options. */
    ios->rearr_opts.comm_type = PIO_REARR_COMM_COLL;
    ios->rearr_opts.fcd = PIO_REARR_COMM_FC_2D_DISABLE;

    /* Copy the computation communicator into union_comm. */
    if ((mpierr = MPI_Comm_dup(comp_comm, &ios->union_comm)))
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

    /* Copy the computation communicator into comp_comm. */
    if ((mpierr = MPI_Comm_dup(comp_comm, &ios->comp_comm)))
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
    PLOG((2, "union_comm = %d comp_comm = %d", ios->union_comm, ios->comp_comm));

    ios->my_comm = ios->comp_comm;
    ustride = stride;

    /* Find MPI rank in comp_comm communicator. */
    if ((mpierr = MPI_Comm_rank(ios->comp_comm, &ios->comp_rank)))
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

    /* With non-async, all tasks are part of computation component. */
    ios->compproc = true;

    /* Create an array that holds the ranks of the tasks to be used
     * for computation. */
    if (!(ios->compranks = calloc(ios->num_comptasks, sizeof(int))))
        return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
    for (int i = 0; i < ios->num_comptasks; i++)
        ios->compranks[i] = i;

    /* Is this the comp master? */
    if (ios->comp_rank == 0)
        ios->compmaster = MPI_ROOT;
    PLOG((2, "comp_rank = %d num_comptasks = %d", ios->comp_rank, ios->num_comptasks));

    /* Create an array that holds the ranks of the tasks to be used
     * for IO. */
    if (!(ios->ioranks = calloc(ios->num_iotasks, sizeof(int))))
        return pio_err(ios, NULL, PIO_ENOMEM, __FILE__, __LINE__);
    for (int i = 0; i < ios->num_iotasks; i++)
    {
        ios->ioranks[i] = (base + i * ustride) % ios->num_comptasks;
        if (ios->ioranks[i] == ios->comp_rank)
            ios->ioproc = true;
        PLOG((3, "ios->ioranks[%d] = %d", i, ios->ioranks[i]));
    }
    ios->ioroot = ios->ioranks[0];

    /* We are not providing an info object. */
    ios->info = MPI_INFO_NULL;

    /* Identify the task that will be the root of the IO communicator. */
    if (ios->comp_rank == ios->ioranks[0])
        ios->iomaster = MPI_ROOT;

    /* Create a group for the computation tasks. */
    if ((mpierr = MPI_Comm_group(ios->comp_comm, &compgroup)))
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

    /* Create a group for the IO tasks. */
    if ((mpierr = MPI_Group_incl(compgroup, ios->num_iotasks, ios->ioranks,
                                 &iogroup)))
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

    /* Create an MPI communicator for the IO tasks. */
    if ((mpierr = MPI_Comm_create(ios->comp_comm, iogroup, &ios->io_comm)))
        return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

    /* Free the MPI groups. */
    if (compgroup != MPI_GROUP_NULL)
        MPI_Group_free(&compgroup);

    if (iogroup != MPI_GROUP_NULL)
        MPI_Group_free(&iogroup);

    /* For the tasks that are doing IO, get their rank within the IO
     * communicator. If they are not doing IO, set their io_rank to
     * -1. */
    if (ios->ioproc)
    {
        if ((mpierr = MPI_Comm_rank(ios->io_comm, &ios->io_rank)))
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
    }
    else
        ios->io_rank = -1;
    PLOG((3, "ios->io_comm = %d ios->io_rank = %d", ios->io_comm, ios->io_rank));

    /* Rank in the union comm is the same as rank in the comp comm. */
    ios->union_rank = ios->comp_rank;

    /* Add this ios struct to the list in the PIO library. */
    *iosysidp = pio_add_to_iosystem_list(ios);

#ifdef USE_MPE
    pio_stop_mpe_log(INIT, __func__);
#endif /* USE_MPE */
    PLOG((2, "Init_Intracomm complete iosysid = %d", *iosysidp));

    return PIO_NOERR;
}

/**
 * Interface to call from pio_init from fortran.
 *
 * @param f90_comp_comm
 * @param num_iotasks the number of IO tasks
 * @param stride the stride to use assigning tasks
 * @param base the starting point when assigning tasks
 * @param rearr the rearranger
 * @param rearr_opts the rearranger options
 * @param iosysidp a pointer that gets the IO system ID
 * @returns 0 for success, error code otherwise
 * @ingroup PIO_init_c
 * @author Jim Edwards
 */
int
PIOc_Init_Intracomm_from_F90(int f90_comp_comm,
                             const int num_iotasks, const int stride,
                             const int base, const int rearr,
                             rearr_opt_t *rearr_opts, int *iosysidp)
{
    int ret = PIO_NOERR;
    ret = PIOc_Init_Intracomm(MPI_Comm_f2c(f90_comp_comm), num_iotasks,
                              stride, base, rearr,
                              iosysidp);
    if (ret != PIO_NOERR)
    {
        PLOG((1, "PIOc_Init_Intracomm failed"));
        return ret;
    }

    if (rearr_opts)
    {
        PLOG((1, "Setting rearranger options, iosys=%d", *iosysidp));
        return PIOc_set_rearr_opts(*iosysidp, rearr_opts->comm_type,
                                   rearr_opts->fcd,
                                   rearr_opts->comp2io.hs,
                                   rearr_opts->comp2io.isend,
                                   rearr_opts->comp2io.max_pend_req,
                                   rearr_opts->io2comp.hs,
                                   rearr_opts->io2comp.isend,
                                   rearr_opts->io2comp.max_pend_req);
    }
    return ret;
}

/**
 * Send a hint to the MPI-IO library.
 *
 * @param iosysid the IO system ID
 * @param hint the hint for MPI
 * @param hintval the value of the hint
 * @returns 0 for success, or PIO_BADID if iosysid can't be found.
 * @ingroup PIO_set_hint_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_set_hint(int iosysid, const char *hint, const char *hintval)
{
    iosystem_desc_t *ios;
    int mpierr; /* Return value for MPI calls. */

    /* Get the iosysid. */
    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    /* User must provide these. */
    if (!hint || !hintval)
        return pio_err(ios, NULL, PIO_EINVAL, __FILE__, __LINE__);

    PLOG((1, "PIOc_set_hint hint = %s hintval = %s", hint, hintval));

    /* Make sure we have an info object. */
    if (ios->info == MPI_INFO_NULL)
        if ((mpierr = MPI_Info_create(&ios->info)))
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

    /* Set the MPI hint. */
    if (ios->ioproc)
        if ((mpierr = MPI_Info_set(ios->info, (char *)hint, (char *)hintval)))
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);

    return PIO_NOERR;
}

/**
 * Clean up internal data structures, and free MPI resources,
 * associated with an IOSystem.
 *
 * @param iosysid: the io system ID provided by PIOc_Init_Intracomm()
 * or PIOc_init_async().
 * @returns 0 for success or non-zero for error.
 * @ingroup PIO_finalize_c
 * @author Jim Edwards, Ed Hartnett
 */
int
PIOc_free_iosystem(int iosysid)
{
    iosystem_desc_t *ios;
    int niosysid;          /* The number of currently open IO systems. */
    int mpierr = MPI_SUCCESS, mpierr2;  /* Return code from MPI function codes. */
    int ierr = PIO_NOERR;

    PLOG((1, "PIOc_finalize iosysid = %d MPI_COMM_NULL = %d", iosysid,
          MPI_COMM_NULL));

    /* Find the IO system information. */
    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    /* If asynch IO is in use, send the PIO_MSG_EXIT message from the
     * comp master to the IO processes. This may be called by
     * componets for other components iosysid. So don't send unless
     * there is a valid union_comm. */
    if (ios->async && ios->union_comm != MPI_COMM_NULL)
    {
        int msg = PIO_MSG_EXIT;

        PLOG((3, "found iosystem info comproot = %d union_comm = %d comp_idx = %d",
              ios->comproot, ios->union_comm, ios->comp_idx));
        if (!ios->ioproc)
        {
            PLOG((2, "sending msg = %d ioroot = %d union_comm = %d", msg,
                  ios->ioroot, ios->union_comm));

            /* Send the message to the message handler. */
            if (ios->compmaster == MPI_ROOT)
                mpierr = MPI_Send(&msg, 1, MPI_INT, ios->ioroot, 1, ios->union_comm);

            /* Send the parameters of the function call. */
            if (!mpierr)
                mpierr = MPI_Bcast((int *)&iosysid, 1, MPI_INT, ios->compmaster, ios->intercomm);
        }

        /* Handle MPI errors. */
        PLOG((3, "handling async errors mpierr = %d my_comm = %d", mpierr, ios->my_comm));
        if ((mpierr2 = MPI_Bcast(&mpierr, 1, MPI_INT, ios->comproot, ios->my_comm)))
            return check_mpi(ios, NULL, mpierr2, __FILE__, __LINE__);
        if (mpierr)
            return check_mpi(ios, NULL, mpierr, __FILE__, __LINE__);
        PLOG((3, "async errors bcast"));
    }

    /* Free this memory that was allocated in init_intracomm. */
    if (ios->ioranks)
        free(ios->ioranks);
    PLOG((3, "Freed ioranks."));
    if (ios->compranks)
        free(ios->compranks);
    PLOG((3, "Freed compranks."));

    /* Learn the number of open IO systems. */
    if ((ierr = pio_num_iosystem(&niosysid)))
        return pio_err(ios, NULL, ierr, __FILE__, __LINE__);
    PLOG((2, "%d iosystems are still open.", niosysid));

    /* Free the MPI communicators. my_comm is just a copy (but not an
     * MPI copy), so does not have to have an MPI_Comm_free()
     * call. comp_comm and io_comm are MPI duplicates of the comms
     * handed into PIOc_init_async(). So they need to be freed by
     * MPI. */
    if (ios->intercomm != MPI_COMM_NULL)
        MPI_Comm_free(&ios->intercomm);
    if (ios->union_comm != MPI_COMM_NULL)
        MPI_Comm_free(&ios->union_comm);
    if (ios->io_comm != MPI_COMM_NULL)
        MPI_Comm_free(&ios->io_comm);
    if (ios->comp_comm != MPI_COMM_NULL)
        MPI_Comm_free(&ios->comp_comm);
    if (ios->my_comm != MPI_COMM_NULL)
        ios->my_comm = MPI_COMM_NULL;

    /* Free the MPI Info object. */
#ifndef _MPISERIAL
    if (ios->info != MPI_INFO_NULL)
        MPI_Info_free(&ios->info);
#endif
    /* Delete the iosystem_desc_t data associated with this id. */
    PLOG((2, "About to delete iosysid %d.", iosysid));
    if ((ierr = pio_delete_iosystem_from_list(iosysid)))
        return pio_err(NULL, NULL, ierr, __FILE__, __LINE__);

    if (niosysid == 1)
    {
        PLOG((1, "about to finalize logging"));
        pio_finalize_logging();
    }

    PLOG((2, "PIOc_finalize completed successfully"));
    return PIO_NOERR;
}

/**
 * Return a logical indicating whether this task is an IO task.
 *
 * @param iosysid the io system ID
 * @param ioproc a pointer that gets 1 if task is an IO task, 0
 * otherwise. Ignored if NULL.
 * @returns 0 for success, or PIO_BADID if iosysid can't be found.
 * @ingroup PIO_iosystem_is_active_c
 * @author Jim Edwards
 */
int
PIOc_iam_iotask(int iosysid, bool *ioproc)
{
    iosystem_desc_t *ios;

    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    if (ioproc)
        *ioproc = ios->ioproc;

    return PIO_NOERR;
}

/**
 * Return the rank of this task in the IO communicator or -1 if this
 * task is not in the communicator.
 *
 * @param iosysid the io system ID
 * @param iorank a pointer that gets the io rank, or -1 if task is not
 * in the IO communicator. Ignored if NULL.
 * @returns 0 for success, or PIO_BADID if iosysid can't be found.
 * @ingroup PIO_iosystem_is_active_c
 * @author Jim Edwards
 */
int
PIOc_iotask_rank(int iosysid, int *iorank)
{
    iosystem_desc_t *ios;

    if (!(ios = pio_get_iosystem_from_id(iosysid)))
        return pio_err(NULL, NULL, PIO_EBADID, __FILE__, __LINE__);

    if (iorank)
        *iorank = ios->io_rank;

    return PIO_NOERR;
}

/**
 * Return true if this iotype is supported in the build, 0 otherwise.
 *
 * @param iotype the io type to check
 * @returns 1 if iotype is in build, 0 if not.
 * @author Jim Edwards
 */
int
PIOc_iotype_available(int iotype)
{
    switch(iotype)
    {
#ifdef _NETCDF4
    case PIO_IOTYPE_NETCDF4P:
    case PIO_IOTYPE_NETCDF4C:
        return 1;
#endif
    case PIO_IOTYPE_NETCDF:
        return 1;
#ifdef _PNETCDF
    case PIO_IOTYPE_PNETCDF:
        return 1;
        break;
#endif
    default:
        return 0;
    }
}

/**
 * Set the target blocksize for the box rearranger.
 *
 * @param newblocksize the new blocksize.
 * @returns 0 for success.
 * @ingroup PIO_set_blocksize_c
 * @author Jim Edwards
 */
int
PIOc_set_blocksize(int newblocksize)
{
    if (newblocksize > 0)
        blocksize = newblocksize;
    return PIO_NOERR;
}
