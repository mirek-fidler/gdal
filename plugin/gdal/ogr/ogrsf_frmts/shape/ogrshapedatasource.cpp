/******************************************************************************
 * $Id: ogrshapedatasource.cpp 28585 2015-03-01 19:42:37Z rouault $
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRShapeDataSource class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999,  Les Technologies SoftMap Inc.
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogrshape.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include <set>

//#define IMMEDIATE_OPENING 1

CPL_CVSID("$Id: ogrshapedatasource.cpp 28585 2015-03-01 19:42:37Z rouault $");


/************************************************************************/
/*                          DS_SHPOpen()                                */
/************************************************************************/

SHPHandle OGRShapeDataSource::DS_SHPOpen( const char * pszShapeFile, const char * pszAccess )
{
    /* Do lazy shx loading for /vsicurl/ */
    if( strncmp(pszShapeFile, "/vsicurl/", strlen("/vsicurl/")) == 0 &&
        strcmp(pszAccess, "r") == 0 )
        pszAccess = "rl";
    SHPHandle hSHP = SHPOpenLL( pszShapeFile, pszAccess, (SAHooks*) VSI_SHP_GetHook(b2GBLimit) );
    if( hSHP != NULL )
        SHPSetFastModeReadObject( hSHP, TRUE );
    return hSHP;
}

/************************************************************************/
/*                           DS_DBFOpen()                               */
/************************************************************************/

DBFHandle OGRShapeDataSource::DS_DBFOpen( const char * pszDBFFile, const char * pszAccess )
{
    DBFHandle hDBF = DBFOpenLL( pszDBFFile, pszAccess, (SAHooks*) VSI_SHP_GetHook(b2GBLimit) );
    return hDBF;
}

/************************************************************************/
/*                         OGRShapeDataSource()                         */
/************************************************************************/

OGRShapeDataSource::OGRShapeDataSource()

{
    pszName = NULL;
    papoLayers = NULL;
    nLayers = 0;
    bSingleFileDataSource = FALSE;
    poPool = new OGRLayerPool();
    b2GBLimit = CSLTestBoolean(CPLGetConfigOption("SHAPE_2GB_LIMIT", "FALSE"));
    papszOpenOptions = NULL;
}


/************************************************************************/
/*                        ~OGRShapeDataSource()                         */
/************************************************************************/

OGRShapeDataSource::~OGRShapeDataSource()

{
    CPLFree( pszName );

    for( int i = 0; i < nLayers; i++ )
    {
        CPLAssert( NULL != papoLayers[i] );

        delete papoLayers[i];
    }

    delete poPool;

    CPLFree( papoLayers );
    CSLDestroy( papszOpenOptions );
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRShapeDataSource::Open( GDALOpenInfo* poOpenInfo,
                              int bTestOpen, int bForceSingleFileDataSource )

{
    CPLAssert( nLayers == 0 );
    
    const char * pszNewName = poOpenInfo->pszFilename;
    int bUpdate = poOpenInfo->eAccess == GA_Update;
    papszOpenOptions = CSLDuplicate( poOpenInfo->papszOpenOptions );
    
    pszName = CPLStrdup( pszNewName );

    bDSUpdate = bUpdate;

    bSingleFileDataSource = bForceSingleFileDataSource;

/* -------------------------------------------------------------------- */
/*      If  bSingleFileDataSource is TRUE we don't try to do anything else.     */
/*      This is only utilized when the OGRShapeDriver::Create()         */
/*      method wants to create a stub OGRShapeDataSource for a          */
/*      single shapefile.  The driver will take care of creating the    */
/*      file by callingICreateLayer().                                  */
/* -------------------------------------------------------------------- */
    if( bSingleFileDataSource )
        return TRUE;
    
/* -------------------------------------------------------------------- */
/*      Is the given path a directory or a regular file?                */
/* -------------------------------------------------------------------- */
    if( !poOpenInfo->bStatOK )
    {
        if( !bTestOpen )
            CPLError( CE_Failure, CPLE_AppDefined,
                   "%s is neither a file or directory, Shape access failed.\n",
                      pszNewName );

        return FALSE;
    }
    
/* -------------------------------------------------------------------- */
/*      Build a list of filenames we figure are Shape files.            */
/* -------------------------------------------------------------------- */
    if( !poOpenInfo->bIsDirectory )
    {
        if( !OpenFile( pszNewName, bUpdate, bTestOpen ) )
        {
            if( !bTestOpen )
                CPLError( CE_Failure, CPLE_OpenFailed,
                          "Failed to open shapefile %s.\n"
                          "It may be corrupt or read-only file accessed in update mode.\n",
                          pszNewName );

            return FALSE;
        }

        bSingleFileDataSource = TRUE;

        return TRUE;
    }
    else
    {
        char      **papszCandidates = CPLReadDir( pszNewName );
        int       iCan, nCandidateCount = CSLCount( papszCandidates );
        int       bMightBeOldCoverage = FALSE;
        std::set<CPLString> osLayerNameSet;

        for( iCan = 0; iCan < nCandidateCount; iCan++ )
        {
            char        *pszFilename;
            const char  *pszCandidate = papszCandidates[iCan];
            const char  *pszLayerName = CPLGetBasename(pszCandidate);
            CPLString osLayerName(pszLayerName);
#ifdef WIN32
            /* On Windows, as filenames are case insensitive, a shapefile layer can be made of */
            /* foo.shp and FOO.DBF, so to detect unique layer names, put them */
            /* upper case in the unique set used for detection */
            osLayerName.toupper();
#endif

            if( EQUAL(pszCandidate,"ARC") )
                bMightBeOldCoverage = TRUE;

            if( strlen(pszCandidate) < 4
                || !EQUAL(pszCandidate+strlen(pszCandidate)-4,".shp") )
                continue;

            pszFilename =
                CPLStrdup(CPLFormFilename(pszNewName, pszCandidate, NULL));

            osLayerNameSet.insert(osLayerName);
#ifdef IMMEDIATE_OPENING
            if( !OpenFile( pszFilename, bUpdate, bTestOpen )
                && !bTestOpen )
            {
                CPLError( CE_Failure, CPLE_OpenFailed,
                          "Failed to open shapefile %s.\n"
                          "It may be corrupt or read-only file accessed in update mode.\n",
                          pszFilename );
                CPLFree( pszFilename );
                CSLDestroy( papszCandidates );
                return FALSE;
            }
#else
            oVectorLayerName.push_back(pszFilename);
#endif
            CPLFree( pszFilename );
        }

        // Try and .dbf files without apparent associated shapefiles. 
        for( iCan = 0; iCan < nCandidateCount; iCan++ )
        {
            char        *pszFilename;
            const char  *pszCandidate = papszCandidates[iCan];
            const char  *pszLayerName = CPLGetBasename(pszCandidate);
            CPLString osLayerName(pszLayerName);
#ifdef WIN32
            osLayerName.toupper();
#endif

            // We don't consume .dbf files in a directory that looks like
            // an old style Arc/Info (for PC?) that unless we found at least
            // some shapefiles.  See Bug 493. 
            if( bMightBeOldCoverage && osLayerNameSet.size() == 0 )
                continue;

            if( strlen(pszCandidate) < 4
                || !EQUAL(pszCandidate+strlen(pszCandidate)-4,".dbf") )
                continue;

            if( osLayerNameSet.find(osLayerName) != osLayerNameSet.end() )
                continue;

            // We don't want to access .dbf files with an associated .tab
            // file, or it will never get recognised as a mapinfo dataset.
            int  iCan2, bFoundTAB = FALSE;
            for( iCan2 = 0; iCan2 < nCandidateCount; iCan2++ )
            {
                const char *pszCandidate2 = papszCandidates[iCan2];

                if( EQUALN(pszCandidate2,pszLayerName,strlen(pszLayerName))
                    && EQUAL(pszCandidate2 + strlen(pszLayerName), ".tab") )
                    bFoundTAB = TRUE;
            }

            if( bFoundTAB )
                continue;

            pszFilename =
                CPLStrdup(CPLFormFilename(pszNewName, pszCandidate, NULL));

            osLayerNameSet.insert(osLayerName);

#ifdef IMMEDIATE_OPENING
            if( !OpenFile( pszFilename, bUpdate, bTestOpen )
                && !bTestOpen )
            {
                CPLError( CE_Failure, CPLE_OpenFailed,
                          "Failed to open dbf file %s.\n"
                          "It may be corrupt or read-only file accessed in update mode.\n",
                          pszFilename );
                CPLFree( pszFilename );
                CSLDestroy( papszCandidates );
                return FALSE;
            }
#else
            oVectorLayerName.push_back(pszFilename);
#endif
            CPLFree( pszFilename );
        }

        CSLDestroy( papszCandidates );

#ifdef IMMEDIATE_OPENING
        int nDirLayers = nLayers;
#else
        int nDirLayers = oVectorLayerName.size();
#endif

        CPLErrorReset();

        return nDirLayers > 0 || !bTestOpen;
    }
}

/************************************************************************/
/*                              OpenFile()                              */
/************************************************************************/

int OGRShapeDataSource::OpenFile( const char *pszNewName, int bUpdate,
                                  int bTestOpen )

{
    SHPHandle   hSHP;
    DBFHandle   hDBF;
    const char *pszExtension = CPLGetExtension( pszNewName );

    (void) bTestOpen;

    if( !EQUAL(pszExtension,"shp") && !EQUAL(pszExtension,"shx")
        && !EQUAL(pszExtension,"dbf") )
        return FALSE;

/* -------------------------------------------------------------------- */
/*      SHPOpen() should include better (CPL based) error reporting,    */
/*      and we should be trying to distinquish at this point whether    */
/*      failure is a result of trying to open a non-shapefile, or       */
/*      whether it was a shapefile and we want to report the error      */
/*      up.                                                             */
/*                                                                      */
/*      Care is taken to suppress the error and only reissue it if      */
/*      we think it is appropriate.                                     */
/* -------------------------------------------------------------------- */
    CPLPushErrorHandler( CPLQuietErrorHandler );
    if( bUpdate )
        hSHP = DS_SHPOpen( pszNewName, "r+" );
    else
        hSHP = DS_SHPOpen( pszNewName, "r" );
    CPLPopErrorHandler();

    if( hSHP == NULL 
        && (!EQUAL(CPLGetExtension(pszNewName),"dbf") 
            || strstr(CPLGetLastErrorMsg(),".shp") == NULL) )
    {
        CPLString osMsg = CPLGetLastErrorMsg();

        CPLError( CE_Failure, CPLE_OpenFailed, "%s", osMsg.c_str() );

        return FALSE;
    }
    CPLErrorReset();
    
/* -------------------------------------------------------------------- */
/*      Open the .dbf file, if it exists.  To open a dbf file, the      */
/*      filename has to either refer to a successfully opened shp       */
/*      file or has to refer to the actual .dbf file.                   */
/* -------------------------------------------------------------------- */
    if( hSHP != NULL || EQUAL(CPLGetExtension(pszNewName),"dbf") )
    {
        if( bUpdate )
        {
            hDBF = DS_DBFOpen( pszNewName, "r+" );
            if( hSHP != NULL && hDBF == NULL )
            {
                for(int i=0;i<2;i++)
                {
                    VSIStatBufL sStat;
                    const char* pszDBFName = CPLResetExtension(pszNewName,
                                                    (i == 0 ) ? "dbf" : "DBF");
                    VSILFILE* fp = NULL;
                    if( VSIStatExL( pszDBFName, &sStat, VSI_STAT_EXISTS_FLAG) == 0 )
                    {
                        fp = VSIFOpenL(pszDBFName, "r+");
                        if (fp == NULL)
                        {
                            CPLError( CE_Failure, CPLE_OpenFailed,
                                    "%s exists, but cannot be opened in update mode",
                                    pszDBFName );
                            SHPClose(hSHP);
                            return FALSE;
                        }
                        VSIFCloseL(fp);
                        break;
                    }
                }
            }
        }
        else
            hDBF = DS_DBFOpen( pszNewName, "r" );
    }
    else
        hDBF = NULL;
        
    if( hDBF == NULL && hSHP == NULL )
        return FALSE;

/* -------------------------------------------------------------------- */
/*      Create the layer object.                                        */
/* -------------------------------------------------------------------- */
    OGRShapeLayer       *poLayer;

    poLayer = new OGRShapeLayer( this, pszNewName, hSHP, hDBF, NULL, FALSE, bUpdate,
                                 wkbNone );
    poLayer->SetModificationDate(
            CSLFetchNameValue( papszOpenOptions, "DBF_DATE_LAST_UPDATE" ) );

/* -------------------------------------------------------------------- */
/*      Add layer to data source layer list.                            */
/* -------------------------------------------------------------------- */
    AddLayer(poLayer);

    return TRUE;
}


/************************************************************************/
/*                             AddLayer()                               */
/************************************************************************/

void OGRShapeDataSource::AddLayer(OGRShapeLayer* poLayer)
{
    papoLayers = (OGRShapeLayer **)
        CPLRealloc( papoLayers,  sizeof(OGRShapeLayer *) * (nLayers+1) );
    papoLayers[nLayers++] = poLayer;

    /* If we reach the limit, then all the already opened layers */
    /* Technically this code would not be necessary if there was not the */
    /* following initial test in SetLastUsedLayer() : */
    /*      if (nLayers < MAX_SIMULTANEOUSLY_OPENED_LAYERS) */
    /*         return; */
    if (nLayers == poPool->GetMaxSimultaneouslyOpened() && poPool->GetSize() == 0)
    {
        for(int i=0;i<nLayers;i++)
            poPool->SetLastUsedLayer(papoLayers[i]);
    }
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRShapeDataSource::ICreateLayer( const char * pszLayerName,
                                 OGRSpatialReference *poSRS,
                                 OGRwkbGeometryType eType,
                                 char ** papszOptions )

{
    SHPHandle   hSHP;
    DBFHandle   hDBF;
    int         nShapeType;

    /* To ensure that existing layers are created */
    GetLayerCount();

/* -------------------------------------------------------------------- */
/*      Check that the layer doesn't already exist.                     */
/* -------------------------------------------------------------------- */
    if (GetLayerByName(pszLayerName) != NULL)
    {
        CPLError( CE_Failure, CPLE_AppDefined, "Layer '%s' already exists",
                    pszLayerName);
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Verify we are in update mode.                                   */
/* -------------------------------------------------------------------- */
    if( !bDSUpdate )
    {
        CPLError( CE_Failure, CPLE_NoWriteAccess,
                  "Data source %s opened read-only.\n"
                  "New layer %s cannot be created.\n",
                  pszName, pszLayerName );

        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Figure out what type of layer we need.                          */
/* -------------------------------------------------------------------- */
    if( eType == wkbUnknown || eType == wkbLineString )
        nShapeType = SHPT_ARC;
    else if( eType == wkbPoint )
        nShapeType = SHPT_POINT;
    else if( eType == wkbPolygon )
        nShapeType = SHPT_POLYGON;
    else if( eType == wkbMultiPoint )
        nShapeType = SHPT_MULTIPOINT;
    else if( eType == wkbPoint25D )
        nShapeType = SHPT_POINTZ;
    else if( eType == wkbLineString25D )
        nShapeType = SHPT_ARCZ;
    else if( eType == wkbMultiLineString )
        nShapeType = SHPT_ARC;
    else if( eType == wkbMultiLineString25D )
        nShapeType = SHPT_ARCZ;
    else if( eType == wkbPolygon25D )
        nShapeType = SHPT_POLYGONZ;
    else if( eType == wkbMultiPolygon )
        nShapeType = SHPT_POLYGON;
    else if( eType == wkbMultiPolygon25D )
        nShapeType = SHPT_POLYGONZ;
    else if( eType == wkbMultiPoint25D )
        nShapeType = SHPT_MULTIPOINTZ;
    else if( eType == wkbNone )
        nShapeType = SHPT_NULL;
    else
        nShapeType = -1;

/* -------------------------------------------------------------------- */
/*      Has the application overridden this with a special creation     */
/*      option?                                                         */
/* -------------------------------------------------------------------- */
    const char *pszOverride = CSLFetchNameValue( papszOptions, "SHPT" );

    if( pszOverride == NULL )
        /* ignore */;
    else if( EQUAL(pszOverride,"POINT") )
    {
        nShapeType = SHPT_POINT;
        eType = wkbPoint;
    }
    else if( EQUAL(pszOverride,"ARC") )
    {
        nShapeType = SHPT_ARC;
        eType = wkbLineString;
    }
    else if( EQUAL(pszOverride,"POLYGON") )
    {
        nShapeType = SHPT_POLYGON;
        eType = wkbPolygon;
    }
    else if( EQUAL(pszOverride,"MULTIPOINT") )
    {
        nShapeType = SHPT_MULTIPOINT;
        eType = wkbMultiPoint;
    }
    else if( EQUAL(pszOverride,"POINTZ") )
    {
        nShapeType = SHPT_POINTZ;
        eType = wkbPoint25D;
    }
    else if( EQUAL(pszOverride,"ARCZ") )
    {
        nShapeType = SHPT_ARCZ;
        eType = wkbLineString25D;
    }
    else if( EQUAL(pszOverride,"POLYGONZ") )
    {
        nShapeType = SHPT_POLYGONZ;
        eType = wkbPolygon25D;
    }
    else if( EQUAL(pszOverride,"MULTIPOINTZ") )
    {
        nShapeType = SHPT_MULTIPOINTZ;
        eType = wkbMultiPoint25D;
    }
    else if( EQUAL(pszOverride,"NONE") || EQUAL(pszOverride,"NULL") )
    {
        nShapeType = SHPT_NULL;
        eType = wkbNone;
    }
    else
    {
        CPLError( CE_Failure, CPLE_NotSupported,
                  "Unknown SHPT value of `%s' passed to Shapefile layer\n"
                  "creation.  Creation aborted.\n",
                  pszOverride );

        return NULL;
    }

    if( nShapeType == -1 )
    {
        CPLError( CE_Failure, CPLE_NotSupported,
                  "Geometry type of `%s' not supported in shapefiles.\n"
                  "Type can be overridden with a layer creation option\n"
                  "of SHPT=POINT/ARC/POLYGON/MULTIPOINT/POINTZ/ARCZ/POLYGONZ/MULTIPOINTZ.\n",
                  OGRGeometryTypeToName(eType) );
        return NULL;
    }
    
/* -------------------------------------------------------------------- */
/*      What filename do we use, excluding the extension?               */
/* -------------------------------------------------------------------- */
    char *pszFilenameWithoutExt;

    if(  bSingleFileDataSource && nLayers == 0 )
    {
        char *pszPath = CPLStrdup(CPLGetPath(pszName));
        char *pszFBasename = CPLStrdup(CPLGetBasename(pszName));

        pszFilenameWithoutExt = CPLStrdup(CPLFormFilename(pszPath, pszFBasename, NULL));

        CPLFree( pszFBasename );
        CPLFree( pszPath );
    }
    else if(  bSingleFileDataSource )
    {
        /* This is a very weird use case : the user creates/open a datasource */
        /* made of a single shapefile 'foo.shp' and wants to add a new layer */
        /* to it, 'bar'. So we create a new shapefile 'bar.shp' in the same */
        /* directory as 'foo.shp' */
        /* So technically, we will not be any longer a single file */
        /* datasource ... Ahem ahem */
        char *pszPath = CPLStrdup(CPLGetPath(pszName));
        pszFilenameWithoutExt = CPLStrdup(CPLFormFilename(pszPath,pszLayerName,NULL));
        CPLFree( pszPath );
    }
    else
        pszFilenameWithoutExt = CPLStrdup(CPLFormFilename(pszName,pszLayerName,NULL));

/* -------------------------------------------------------------------- */
/*      Create the shapefile.                                           */
/* -------------------------------------------------------------------- */
    char        *pszFilename;

    int b2GBLimit = CSLTestBoolean(CSLFetchNameValueDef( papszOptions, "2GB_LIMIT", "FALSE" ));

    if( nShapeType != SHPT_NULL )
    {
        pszFilename = CPLStrdup(CPLFormFilename( NULL, pszFilenameWithoutExt, "shp" ));

        hSHP = SHPCreateLL( pszFilename, nShapeType, (SAHooks*) VSI_SHP_GetHook(b2GBLimit) );
        
        if( hSHP == NULL )
        {
            CPLError( CE_Failure, CPLE_OpenFailed,
                      "Failed to open Shapefile `%s'.\n",
                      pszFilename );
            CPLFree( pszFilename );
            CPLFree( pszFilenameWithoutExt );
            return NULL;
        }
        
        SHPSetFastModeReadObject( hSHP, TRUE );

        CPLFree( pszFilename );
    }
    else
        hSHP = NULL;

/* -------------------------------------------------------------------- */
/*      Has a specific LDID been specified by the caller?               */
/* -------------------------------------------------------------------- */
    const char *pszLDID = CSLFetchNameValue( papszOptions, "ENCODING" );

/* -------------------------------------------------------------------- */
/*      Create a DBF file.                                              */
/* -------------------------------------------------------------------- */
    pszFilename = CPLStrdup(CPLFormFilename( NULL, pszFilenameWithoutExt, "dbf" ));

    if( pszLDID != NULL )
        hDBF = DBFCreateLL( pszFilename, pszLDID, (SAHooks*) VSI_SHP_GetHook(b2GBLimit) );
    else
        hDBF = DBFCreateLL( pszFilename, "LDID/87",(SAHooks*) VSI_SHP_GetHook(b2GBLimit) );

    if( hDBF == NULL )
    {
        CPLError( CE_Failure, CPLE_OpenFailed,
                  "Failed to open Shape DBF file `%s'.\n",
                  pszFilename );
        CPLFree( pszFilename );
        CPLFree( pszFilenameWithoutExt );
        SHPClose(hSHP);
        return NULL;
    }

    CPLFree( pszFilename );

/* -------------------------------------------------------------------- */
/*      Create the .prj file, if required.                              */
/* -------------------------------------------------------------------- */
    if( poSRS != NULL )
    {
        char    *pszWKT = NULL;
        CPLString osPrjFile = CPLFormFilename( NULL, pszFilenameWithoutExt, "prj");
        VSILFILE    *fp;

        /* the shape layer needs it's own copy */
        poSRS = poSRS->Clone();
        poSRS->morphToESRI();

        if( poSRS->exportToWkt( &pszWKT ) == OGRERR_NONE 
            && (fp = VSIFOpenL( osPrjFile, "wt" )) != NULL )
        {
            VSIFWriteL( pszWKT, strlen(pszWKT), 1, fp );
            VSIFCloseL( fp );
        }

        CPLFree( pszWKT );

        poSRS->morphFromESRI();
    }

/* -------------------------------------------------------------------- */
/*      Create the layer object.                                        */
/* -------------------------------------------------------------------- */
    OGRShapeLayer       *poLayer;

    /* OGRShapeLayer constructor expects a filename with an extension (that could be */
    /* random actually), otherwise this is going to cause problems with layer */
    /* names that have a dot (not speaking about the one before the shp) */
    pszFilename = CPLStrdup(CPLFormFilename( NULL, pszFilenameWithoutExt, "shp" ));

    poLayer = new OGRShapeLayer( this, pszFilename, hSHP, hDBF, poSRS, TRUE, TRUE,
                                 eType );

    CPLFree( pszFilenameWithoutExt );
    CPLFree( pszFilename );

    poLayer->SetResizeAtClose( CSLFetchBoolean( papszOptions, "RESIZE", FALSE ) );
    poLayer->CreateSpatialIndexAtClose( CSLFetchBoolean( papszOptions, "SPATIAL_INDEX", FALSE ) );
    poLayer->SetModificationDate(
        CSLFetchNameValue( papszOptions, "DBF_DATE_LAST_UPDATE" ) );

/* -------------------------------------------------------------------- */
/*      Add layer to data source layer list.                            */
/* -------------------------------------------------------------------- */
    AddLayer(poLayer);

    return poLayer;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRShapeDataSource::TestCapability( const char * pszCap )

{
    if( EQUAL(pszCap,ODsCCreateLayer) )
        return bDSUpdate;
    else if( EQUAL(pszCap,ODsCDeleteLayer) )
        return bDSUpdate;
    else
        return FALSE;
}

/************************************************************************/
/*                            GetLayerCount()                           */
/************************************************************************/

int OGRShapeDataSource::GetLayerCount()

{
#ifndef IMMEDIATE_OPENING
    if (oVectorLayerName.size() != 0)
    {
        for(size_t i = 0; i < oVectorLayerName.size(); i++)
        {
            const char* pszFilename = oVectorLayerName[i].c_str();
            const char* pszLayerName = CPLGetBasename(pszFilename);

            int j;
            for(j=0;j<nLayers;j++)
            {
                if (strcmp(papoLayers[j]->GetName(), pszLayerName) == 0)
                    break;
            }
            if (j < nLayers)
                continue;

            if( !OpenFile( pszFilename, bDSUpdate, TRUE ) )
            {
                CPLError( CE_Failure, CPLE_OpenFailed,
                          "Failed to open file %s.\n"
                          "It may be corrupt or read-only file accessed in update mode.\n",
                          pszFilename );
            }
        }
        oVectorLayerName.resize(0);
    }
#endif

    return nLayers;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRShapeDataSource::GetLayer( int iLayer )

{
    /* To ensure that existing layers are created */
    GetLayerCount();

    if( iLayer < 0 || iLayer >= nLayers )
        return NULL;
    else
        return papoLayers[iLayer];
}

/************************************************************************/
/*                           GetLayerByName()                           */
/************************************************************************/

OGRLayer *OGRShapeDataSource::GetLayerByName(const char * pszLayerNameIn)
{
#ifndef IMMEDIATE_OPENING
    if (oVectorLayerName.size() != 0)
    {
        int j;
        for(j=0;j<nLayers;j++)
        {
            if (strcmp(papoLayers[j]->GetName(), pszLayerNameIn) == 0)
            {
                return papoLayers[j];
            }
        }

        size_t i;
        for(j = 0; j < 2; j++)
        {
            for(i = 0; i < oVectorLayerName.size(); i++)
            {
                const char* pszFilename = oVectorLayerName[i].c_str();
                const char* pszLayerName = CPLGetBasename(pszFilename);

                if (j == 0)
                {
                    if (strcmp(pszLayerName, pszLayerNameIn) != 0)
                        continue;
                }
                else
                {
                    if ( !EQUAL(pszLayerName, pszLayerNameIn) )
                        continue;
                }

                if( !OpenFile( pszFilename, bDSUpdate, TRUE ) )
                {
                    CPLError( CE_Failure, CPLE_OpenFailed,
                            "Failed to open file %s.\n"
                            "It may be corrupt or read-only file accessed in update mode.\n",
                            pszFilename );
                    return NULL;
                }
                else
                {
                    return papoLayers[nLayers - 1];
                }
            }
        }

        return NULL;
    }
#endif

    return OGRDataSource::GetLayerByName(pszLayerNameIn);
}

/************************************************************************/
/*                             ExecuteSQL()                             */
/*                                                                      */
/*      We override this to provide special handling of CREATE          */
/*      SPATIAL INDEX commands.  Support forms are:                     */
/*                                                                      */
/*        CREATE SPATIAL INDEX ON layer_name [DEPTH n]                  */
/*        DROP SPATIAL INDEX ON layer_name                              */
/*        REPACK layer_name                                             */
/*        RECOMPUTE EXTENT ON layer_name                                */
/************************************************************************/

OGRLayer * OGRShapeDataSource::ExecuteSQL( const char *pszStatement,
                                           OGRGeometry *poSpatialFilter,
                                           const char *pszDialect )

{
/* ==================================================================== */
/*      Handle command to drop a spatial index.                         */
/* ==================================================================== */
    if( EQUALN(pszStatement, "REPACK ", 7) )
    {
        OGRShapeLayer *poLayer = (OGRShapeLayer *) 
            GetLayerByName( pszStatement + 7 );

        if( poLayer != NULL )
        {
            if( poLayer->Repack() != OGRERR_NONE )
            {
                CPLError( CE_Failure, CPLE_AppDefined,
                          "REPACK of layer '%s' failed.",
                          pszStatement + 7 );
            }
        }
        else
        {
            CPLError( CE_Failure, CPLE_AppDefined, 
                      "No such layer as '%s' in REPACK.", 
                      pszStatement + 7 );
        }
        return NULL;
    }

/* ==================================================================== */
/*      Handle command to shrink columns to their minimum size.         */
/* ==================================================================== */
    if( EQUALN(pszStatement, "RESIZE ", 7) )
    {
        OGRShapeLayer *poLayer = (OGRShapeLayer *)
            GetLayerByName( pszStatement + 7 );

        if( poLayer != NULL )
            poLayer->ResizeDBF();
        else
        {
            CPLError( CE_Failure, CPLE_AppDefined,
                      "No such layer as '%s' in RESIZE.",
                      pszStatement + 7 );
        }
        return NULL;
    }

/* ==================================================================== */
/*      Handle command to recompute extent                             */
/* ==================================================================== */
    if( EQUALN(pszStatement, "RECOMPUTE EXTENT ON ", 20) )
    {
        OGRShapeLayer *poLayer = (OGRShapeLayer *) 
            GetLayerByName( pszStatement + 20 );

        if( poLayer != NULL )
            poLayer->RecomputeExtent();
        else
        {
            CPLError( CE_Failure, CPLE_AppDefined, 
                      "No such layer as '%s' in RECOMPUTE EXTENT.", 
                      pszStatement + 20 );
        }
        return NULL;
    }
    
/* ==================================================================== */
/*      Handle command to drop a spatial index.                         */
/* ==================================================================== */
    if( EQUALN(pszStatement, "DROP SPATIAL INDEX ON ", 22) )
    {
        OGRShapeLayer *poLayer = (OGRShapeLayer *) 
            GetLayerByName( pszStatement + 22 );

        if( poLayer != NULL )
            poLayer->DropSpatialIndex();
        else
        {
            CPLError( CE_Failure, CPLE_AppDefined, 
                      "No such layer as '%s' in DROP SPATIAL INDEX.", 
                      pszStatement + 22 );
        }
        return NULL;
    }
    
/* ==================================================================== */
/*      Handle all comands except spatial index creation generically.   */
/* ==================================================================== */
    if( !EQUALN(pszStatement,"CREATE SPATIAL INDEX ON ",24) )
    {
        char **papszTokens = CSLTokenizeString( pszStatement );
        if( CSLCount(papszTokens) >=4
            && (EQUAL(papszTokens[0],"CREATE") || EQUAL(papszTokens[0],"DROP"))
            && EQUAL(papszTokens[1],"INDEX")
            && EQUAL(papszTokens[2],"ON") )
        {
            OGRShapeLayer *poLayer = (OGRShapeLayer *) GetLayerByName(papszTokens[3]);
            if (poLayer != NULL)
                poLayer->InitializeIndexSupport( poLayer->GetFullName() );
        }
        CSLDestroy( papszTokens );

        return OGRDataSource::ExecuteSQL( pszStatement, poSpatialFilter, 
                                          pszDialect );
    }

/* -------------------------------------------------------------------- */
/*      Parse into keywords.                                            */
/* -------------------------------------------------------------------- */
    char **papszTokens = CSLTokenizeString( pszStatement );
    
    if( CSLCount(papszTokens) < 5
        || !EQUAL(papszTokens[0],"CREATE")
        || !EQUAL(papszTokens[1],"SPATIAL")
        || !EQUAL(papszTokens[2],"INDEX") 
        || !EQUAL(papszTokens[3],"ON") 
        || CSLCount(papszTokens) > 7 
        || (CSLCount(papszTokens) == 7 && !EQUAL(papszTokens[5],"DEPTH")) )
    {
        CSLDestroy( papszTokens );
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Syntax error in CREATE SPATIAL INDEX command.\n"
                  "Was '%s'\n"
                  "Should be of form 'CREATE SPATIAL INDEX ON <table> [DEPTH <n>]'",
                  pszStatement );
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Get depth if provided.                                          */
/* -------------------------------------------------------------------- */
    int nDepth = 0;
    if( CSLCount(papszTokens) == 7 )
        nDepth = atoi(papszTokens[6]);

/* -------------------------------------------------------------------- */
/*      What layer are we operating on.                                 */
/* -------------------------------------------------------------------- */
    OGRShapeLayer *poLayer = (OGRShapeLayer *) GetLayerByName(papszTokens[4]);

    if( poLayer == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Layer %s not recognised.", 
                  papszTokens[4] );
        CSLDestroy( papszTokens );
        return NULL;
    }

    CSLDestroy( papszTokens );

    poLayer->CreateSpatialIndex( nDepth );
    return NULL;
}


/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

OGRErr OGRShapeDataSource::DeleteLayer( int iLayer )

{
    char *pszFilename;

/* -------------------------------------------------------------------- */
/*      Verify we are in update mode.                                   */
/* -------------------------------------------------------------------- */
    if( !bDSUpdate )
    {
        CPLError( CE_Failure, CPLE_NoWriteAccess,
                  "Data source %s opened read-only.\n"
                  "Layer %d cannot be deleted.\n",
                  pszName, iLayer );

        return OGRERR_FAILURE;
    }

    if( iLayer < 0 || iLayer >= nLayers )
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Layer %d not in legal range of 0 to %d.", 
                  iLayer, nLayers-1 );
        return OGRERR_FAILURE;
    }

    OGRShapeLayer* poLayerToDelete = (OGRShapeLayer*) papoLayers[iLayer];

    pszFilename = CPLStrdup(poLayerToDelete->GetFullName());

    delete poLayerToDelete;

    while( iLayer < nLayers - 1 )
    {
        papoLayers[iLayer] = papoLayers[iLayer+1];
        iLayer++;
    }

    nLayers--;

    VSIUnlink( CPLResetExtension(pszFilename, "shp") );
    VSIUnlink( CPLResetExtension(pszFilename, "shx") );
    VSIUnlink( CPLResetExtension(pszFilename, "dbf") );
    VSIUnlink( CPLResetExtension(pszFilename, "prj") );
    VSIUnlink( CPLResetExtension(pszFilename, "qix") );

    CPLFree( pszFilename );

    return OGRERR_NONE;
}

/************************************************************************/
/*                          SetLastUsedLayer()                          */
/************************************************************************/

void OGRShapeDataSource::SetLastUsedLayer( OGRShapeLayer* poLayer )
{
    /* We could remove that check and things would still work in */
    /* 99.99% cases */
    /* The only rationale for that test is to avoid breaking applications */
    /* that would deal with layers of the same datasource in different */
    /* threads. In GDAL < 1.9.0, this would work in most cases I can */
    /* imagine as shapefile layers are pretty much independant from each */
    /* others (although it has never been guaranteed to be a valid use case, */
    /* and the shape driver is likely more the exception than the rule in */
    /* permitting accessing layers from different threads !) */
    /* Anyway the LRU list mechanism leaves the door open to concurrent accesses to it */
    /* so when the datasource has not many layers, we don't try to build the */
    /* LRU list to avoid concurrency issues. I haven't bothered making the analysis */
    /* of how a mutex could be used to protect that (my intuition is that it would */
    /* need to be placed at the beginning of OGRShapeLayer::TouchLayer() ) */
    if (nLayers < poPool->GetMaxSimultaneouslyOpened())
        return;

    poPool->SetLastUsedLayer(poLayer);
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char** OGRShapeDataSource::GetFileList()
{
    CPLStringList       oFileList;
    GetLayerCount();
    for(int i=0;i<nLayers;i++)
    {
        OGRShapeLayer* poLayer = papoLayers[i];
        poLayer->AddToFileList(oFileList);
    }
    return oFileList.StealList();
}
