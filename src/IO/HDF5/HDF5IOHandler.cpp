/* Copyright 2017 Fabian Koller
 *
 * This file is part of openPMD-api.
 *
 * openPMD-api is free software: you can redistribute it and/or modify
 * it under the terms of of either the GNU General Public License or
 * the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * openPMD-api is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License and the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * and the GNU Lesser General Public License along with openPMD-api.
 * If not, see <http://www.gnu.org/licenses/>.
 */
#include "openPMD/IO/HDF5/HDF5IOHandler.hpp"


#if defined(openPMD_HAVE_HDF5)
#   include "openPMD/auxiliary/StringManip.hpp"
#   include "openPMD/backend/Attribute.hpp"
#   include "openPMD/IO/IOTask.hpp"
#   include "openPMD/IO/HDF5/HDF5Auxiliary.hpp"
#   include "openPMD/IO/HDF5/HDF5FilePosition.hpp"
#endif

#include <boost/filesystem.hpp>

#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace openPMD
{
#if defined(openPMD_HAVE_HDF5)
#   ifdef DEBUG
#       define ASSERT(CONDITION, TEXT) { if(!(CONDITION)) throw std::runtime_error(std::string((TEXT))); }
#   else
#       define ASSERT(CONDITION, TEXT) do{ (void)sizeof(CONDITION); } while( 0 )
#   endif

HDF5IOHandlerImpl::HDF5IOHandlerImpl(AbstractIOHandler* handler)
        : m_datasetTransferProperty{H5P_DEFAULT},
          m_fileAccessProperty{H5P_DEFAULT},
          m_H5T_BOOL_ENUM{H5Tenum_create(H5T_NATIVE_INT8)},
          m_handler{handler}
{
    ASSERT(m_H5T_BOOL_ENUM >= 0, "Internal error: Failed to create HDF5 enum");
    std::string t{"TRUE"};
    std::string f{"FALSE"};
    int64_t tVal = 1;
    int64_t fVal = 0;
    herr_t status;
    status = H5Tenum_insert(m_H5T_BOOL_ENUM, t.c_str(), &tVal);
    ASSERT(status == 0, "Internal error: Failed to insert into HDF5 enum");
    status = H5Tenum_insert(m_H5T_BOOL_ENUM, f.c_str(), &fVal);
    ASSERT(status == 0, "Internal error: Failed to insert into HDF5 enum");
}

HDF5IOHandlerImpl::~HDF5IOHandlerImpl()
{
    herr_t status;
    status = H5Tclose(m_H5T_BOOL_ENUM);
    if( status < 0 )
        std::cerr << "Internal error: Failed to close HDF5 enum\n";
    while( !m_openFileIDs.empty() )
    {
        auto file = m_openFileIDs.begin();
        status = H5Fclose(*file);
        if( status < 0 )
            std::cerr << "Internal error: Failed to close HDF5 file (serial)\n";
        m_openFileIDs.erase(file);
    }
    if( m_datasetTransferProperty != H5P_DEFAULT )
    {
        status = H5Pclose(m_datasetTransferProperty);
        if( status < 0 )
            std::cerr <<  "Internal error: Failed to close HDF5 dataset transfer property\n";
    }
    if( m_fileAccessProperty != H5P_DEFAULT )
    {
        status = H5Pclose(m_fileAccessProperty);
        if( status < 0 )
            std::cerr << "Internal error: Failed to close HDF5 file access property\n";
    }
}

std::future< void >
HDF5IOHandlerImpl::flush()
{
    while( !(*m_handler).m_work.empty() )
    {
        IOTask& i = (*m_handler).m_work.front();
        try
        {
            switch( i.operation )
            {
                using O = Operation;
                case O::CREATE_FILE:
                    createFile(i.writable, i.parameter);
                    break;
                case O::CREATE_PATH:
                    createPath(i.writable, i.parameter);
                    break;
                case O::CREATE_DATASET:
                    createDataset(i.writable, i.parameter);
                    break;
                case O::EXTEND_DATASET:
                    extendDataset(i.writable, i.parameter);
                    break;
                case O::OPEN_FILE:
                    openFile(i.writable, i.parameter);
                    break;
                case O::OPEN_PATH:
                    openPath(i.writable, i.parameter);
                    break;
                case O::OPEN_DATASET:
                    openDataset(i.writable, i.parameter);
                    break;
                case O::DELETE_FILE:
                    deleteFile(i.writable, i.parameter);
                    break;
                case O::DELETE_PATH:
                    deletePath(i.writable, i.parameter);
                    break;
                case O::DELETE_DATASET:
                    deleteDataset(i.writable, i.parameter);
                    break;
                case O::DELETE_ATT:
                    deleteAttribute(i.writable, i.parameter);
                    break;
                case O::WRITE_DATASET:
                    writeDataset(i.writable, i.parameter);
                    break;
                case O::WRITE_ATT:
                    writeAttribute(i.writable, i.parameter);
                    break;
                case O::READ_DATASET:
                    readDataset(i.writable, i.parameter);
                    break;
                case O::READ_ATT:
                    readAttribute(i.writable, i.parameter);
                    break;
                case O::LIST_PATHS:
                    listPaths(i.writable, i.parameter);
                    break;
                case O::LIST_DATASETS:
                    listDatasets(i.writable, i.parameter);
                    break;
                case O::LIST_ATTS:
                    listAttributes(i.writable, i.parameter);
                    break;
            }
        } catch (unsupported_data_error& e)
        {
            (*m_handler).m_work.pop();
            throw e;
        }
        (*m_handler).m_work.pop();
    }
    return std::future< void >();
}

void
HDF5IOHandlerImpl::createFile(Writable* writable,
                              ArgumentMap const& parameters)
{
    if( !writable->written )
    {
        using namespace boost::filesystem;
        path dir(m_handler->directory);
        if( !exists(dir) )
            create_directories(dir);

        /* Create a new file using current properties. */
        std::string name = m_handler->directory + parameters.at("name").get< std::string >();
        if( !auxiliary::ends_with(name, ".h5") )
            name += ".h5";
        hid_t id = H5Fcreate(name.c_str(),
                             H5F_ACC_TRUNC,
                             H5P_DEFAULT,
                             m_fileAccessProperty);
        ASSERT(id >= 0, "Internal error: Failed to create HDF5 file");

        writable->written = true;
        writable->abstractFilePosition = std::make_shared< HDF5FilePosition >("/");

        m_fileIDs[writable] = id;
        m_openFileIDs.insert(id);
    }
}

void
HDF5IOHandlerImpl::createPath(Writable* writable,
                              ArgumentMap const& parameters)
{
    if( !writable->written )
    {
        /* Sanitize path */
        std::string path = parameters.at("path").get< std::string >();
        if( auxiliary::starts_with(path, "/") )
            path = auxiliary::replace_first(path, "/", "");
        if( !auxiliary::ends_with(path, "/") )
            path += '/';

        /* Open H5Object to write into */
        Writable* position;
        if( writable->parent )
            position = writable->parent;
        else
            position = writable; /* root does not have a parent but might still have to be written */
        auto res = m_fileIDs.find(position);
        hid_t node_id = H5Gopen(res->second,
                                concrete_h5_file_position(position).c_str(),
                                H5P_DEFAULT);
        ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during path creation");

        /* Create the path in the file */
        std::stack< hid_t > groups;
        groups.push(node_id);
        for( std::string const& folder : auxiliary::split(path, "/", false) )
        {
            hid_t group_id = H5Gcreate(groups.top(),
                                       folder.c_str(),
                                       H5P_DEFAULT,
                                       H5P_DEFAULT,
                                       H5P_DEFAULT);
            ASSERT(group_id >= 0, "Internal error: Failed to create HDF5 group during path creation");
            groups.push(group_id);
        }

        /* Close the groups */
        herr_t status;
        while( !groups.empty() )
        {
            status = H5Gclose(groups.top());
            ASSERT(status == 0, "Internal error: Failed to close HDF5 group during path creation");
            groups.pop();
        }

        writable->written = true;
        writable->abstractFilePosition = std::make_shared< HDF5FilePosition >(path);

        m_fileIDs[writable] = res->second;
    }
}

void
HDF5IOHandlerImpl::createDataset(Writable* writable,
                                 ArgumentMap const& parameters)
{
    if( !writable->written )
    {
        std::string name = parameters.at("name").get< std::string >();
        if( auxiliary::starts_with(name, "/") )
            name = auxiliary::replace_first(name, "/", "");
        if( auxiliary::ends_with(name, "/") )
            name = auxiliary::replace_first(name, "/", "");


        /* Open H5Object to write into */
        auto res = m_fileIDs.find(writable);
        if( res == m_fileIDs.end() )
            res = m_fileIDs.find(writable->parent);
        hid_t node_id = H5Gopen(res->second,
                                concrete_h5_file_position(writable).c_str(),
                                H5P_DEFAULT);
        ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during dataset creation");

        Datatype d = parameters.at("dtype").get< Datatype >();
        if( d == Datatype::UNDEFINED )
        {
            // TODO handle unknown dtype
            std::cerr << "Datatype::UNDEFINED caught during dataset creation (serial HDF5)" << std::endl;
            d = Datatype::BOOL;
        }
        Attribute a(0);
        a.dtype = d;
        std::vector< hsize_t > dims;
        std::vector< hsize_t > maxdims;
        for( auto const& val : parameters.at("extent").get< Extent >() )
        {
            dims.push_back(static_cast< hsize_t >(val));
            maxdims.push_back(H5S_UNLIMITED);
        }

        hid_t space = H5Screate_simple(dims.size(), dims.data(), maxdims.data());

        std::vector< hsize_t > chunkDims;
        for( auto const& val : parameters.at("chunkSize").get< Extent >() )
            chunkDims.push_back(static_cast< hsize_t >(val));

        /* enable chunking on the created dataspace */
        hid_t datasetCreationProperty = H5Pcreate(H5P_DATASET_CREATE);
        herr_t status;
        status = H5Pset_chunk(datasetCreationProperty, chunkDims.size(), chunkDims.data());
        ASSERT(status == 0, "Internal error: Failed to set chunk size during dataset creation");

        std::string const& compression = parameters.at("compression").get< std::string >();
        if( !compression.empty() )
        {
            std::vector< std::string > args = auxiliary::split(compression, ":");
            std::string const& format = args[0];
            if( (format == "zlib" || format == "gzip" || format == "deflate")
                && args.size() == 2 )
            {
                status = H5Pset_deflate(datasetCreationProperty, std::stoi(args[1]));
                ASSERT(status == 0, "Internal error: Failed to set deflate compression during dataset creation");
            } else if( format == "szip" || format == "nbit" || format == "scaleoffset" )
                std::cerr << "Compression format " << format
                          << " not yet implemented. Data will not be compressed!"
                          << std::endl;
            else
                std::cerr << "Compression format " << format
                          << " unknown. Data will not be compressed!"
                          << std::endl;
        }

        std::string const& transform = parameters.at("transform").get< std::string >();
        if( !transform.empty() )
            std::cerr << "Custom transform not yet implemented in HDF5 backend."
                      << std::endl;

        hid_t datatype = getH5DataType(a);
        ASSERT(datatype >= 0, "Internal error: Failed to get HDF5 datatype during dataset creation");
        hid_t group_id = H5Dcreate(node_id,
                                   name.c_str(),
                                   datatype,
                                   space,
                                   H5P_DEFAULT,
                                   datasetCreationProperty,
                                   H5P_DEFAULT);
        ASSERT(group_id >= 0, "Internal error: Failed to create HDF5 group during dataset creation");

        status = H5Dclose(group_id);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 dataset during dataset creation");
        status = H5Tclose(datatype);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 datatype during dataset creation");
        status = H5Pclose(datasetCreationProperty);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 dataset creation property during dataset creation");
        status = H5Sclose(space);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 dataset space during dataset creation");
        status = H5Gclose(node_id);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 group during dataset creation");

        writable->written = true;
        writable->abstractFilePosition = std::make_shared< HDF5FilePosition >(name);

        m_fileIDs[writable] = res->second;
    }
}

void
HDF5IOHandlerImpl::extendDataset(Writable* writable,
                                 ArgumentMap const& parameters)
{
    if( !writable->written )
        throw std::runtime_error("Extending an unwritten Dataset is not possible.");

    auto res = m_fileIDs.find(writable->parent);
    hid_t node_id, dataset_id;
    node_id = H5Gopen(res->second,
                      concrete_h5_file_position(writable->parent).c_str(),
                      H5P_DEFAULT);
    ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during dataset extension");

    /* Sanitize name */
    std::string name = parameters.at("name").get< std::string >();
    if( auxiliary::starts_with(name, "/") )
        name = auxiliary::replace_first(name, "/", "");
    if( !auxiliary::ends_with(name, "/") )
        name += '/';

    dataset_id = H5Dopen(node_id,
                         name.c_str(),
                         H5P_DEFAULT);
    ASSERT(dataset_id >= 0, "Internal error: Failed to open HDF5 dataset during dataset extension");

    std::vector< hsize_t > size;
    for( auto const& val : parameters.at("extent").get< Extent >() )
        size.push_back(static_cast< hsize_t >(val));

    herr_t status;
    status = H5Dset_extent(dataset_id, size.data());
    ASSERT(status == 0, "Internal error: Failed to extend HDF5 dataset during dataset extension");

    status = H5Dclose(dataset_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 dataset during dataset extension");
    status = H5Gclose(node_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 group during dataset extension");
}

void
HDF5IOHandlerImpl::openFile(Writable* writable,
                            ArgumentMap const& parameters)
{
    //TODO check if file already open
    //not possible with current implementation
    //quick idea - map with filenames as key
    using namespace boost::filesystem;
    path dir(m_handler->directory);
    if( !exists(dir) )
        throw no_such_file_error("Supplied directory is not valid: " + m_handler->directory);

    std::string name = m_handler->directory + parameters.at("name").get< std::string >();
    if( !auxiliary::ends_with(name, ".h5") )
        name += ".h5";

    unsigned flags;
    AccessType at = m_handler->accessType;
    if( at == AccessType::READ_ONLY )
        flags = H5F_ACC_RDONLY;
    else if( at == AccessType::READ_WRITE || at == AccessType::CREATE )
        flags = H5F_ACC_RDWR;
    else
        throw std::runtime_error("Unknown file AccessType");
    hid_t file_id;
    file_id = H5Fopen(name.c_str(),
                      flags,
                      m_fileAccessProperty);
    if( file_id < 0 )
        throw no_such_file_error("Failed to open HDF5 file " + name);

    writable->written = true;
    writable->abstractFilePosition = std::make_shared< HDF5FilePosition >("/");

    m_fileIDs.erase(writable);
    m_fileIDs.insert({writable, file_id});
    m_openFileIDs.insert(file_id);
}

void
HDF5IOHandlerImpl::openPath(Writable* writable,
                            ArgumentMap const& parameters)
{
    auto res = m_fileIDs.find(writable->parent);
    hid_t node_id, path_id;
    node_id = H5Gopen(res->second,
                      concrete_h5_file_position(writable->parent).c_str(),
                      H5P_DEFAULT);
    ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during path opening");

    /* Sanitize path */
    std::string path = parameters.at("path").get< std::string >();
    if( auxiliary::starts_with(path, "/") )
        path = auxiliary::replace_first(path, "/", "");
    if( !auxiliary::ends_with(path, "/") )
        path += '/';

    path_id = H5Gopen(node_id,
                      path.c_str(),
                      H5P_DEFAULT);
    ASSERT(path_id >= 0, "Internal error: Failed to open HDF5 group during path opening");

    herr_t status;
    status = H5Gclose(path_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 group during path opening");
    status = H5Gclose(node_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 group during path opening");

    writable->written = true;
    writable->abstractFilePosition = std::make_shared< HDF5FilePosition >(path);

    m_fileIDs.erase(writable);
    m_fileIDs.insert({writable, res->second});
}

void
HDF5IOHandlerImpl::openDataset(Writable* writable,
                               std::map< std::string, ParameterArgument > & parameters)
{
    auto res = m_fileIDs.find(writable->parent);
    hid_t node_id, dataset_id;
    node_id = H5Gopen(res->second,
                      concrete_h5_file_position(writable->parent).c_str(),
                      H5P_DEFAULT);
    ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during dataset opening");

    /* Sanitize name */
    std::string name = parameters.at("name").get< std::string >();
    if( auxiliary::starts_with(name, "/") )
        name = auxiliary::replace_first(name, "/", "");
    if( !auxiliary::ends_with(name, "/") )
        name += '/';

    dataset_id = H5Dopen(node_id,
                         name.c_str(),
                         H5P_DEFAULT);
    ASSERT(dataset_id >= 0, "Internal error: Failed to open HDF5 dataset during dataset opening");

    hid_t dataset_type, dataset_space;
    dataset_type = H5Dget_type(dataset_id);
    dataset_space = H5Dget_space(dataset_id);

    H5S_class_t dataset_class = H5Sget_simple_extent_type(dataset_space);

    using DT = Datatype;
    Datatype d;
    if( dataset_class == H5S_SIMPLE || dataset_class == H5S_SCALAR )
    {
        if( H5Tequal(dataset_type, H5T_NATIVE_CHAR) )
            d = DT::CHAR;
        else if( H5Tequal(dataset_type, H5T_NATIVE_UCHAR) )
            d = DT::UCHAR;
        else if( H5Tequal(dataset_type, H5T_NATIVE_INT16) )
            d = DT::INT16;
        else if( H5Tequal(dataset_type, H5T_NATIVE_INT32) )
            d = DT::INT32;
        else if( H5Tequal(dataset_type, H5T_NATIVE_INT64) )
            d = DT::INT64;
        else if( H5Tequal(dataset_type, H5T_NATIVE_FLOAT) )
            d = DT::FLOAT;
        else if( H5Tequal(dataset_type, H5T_NATIVE_DOUBLE) )
            d = DT::DOUBLE;
        else if( H5Tequal(dataset_type, H5T_NATIVE_UINT16) )
            d = DT::UINT16;
        else if( H5Tequal(dataset_type, H5T_NATIVE_UINT32) )
            d = DT::UINT32;
        else if( H5Tequal(dataset_type, H5T_NATIVE_UINT64) )
            d = DT::UINT64;
        else if( H5Tget_class(dataset_type) == H5T_STRING )
            d = DT::STRING;
        else
            throw std::runtime_error("Unknown dataset type");
    } else
        throw std::runtime_error("Unsupported dataset class");

    auto dtype = parameters.at("dtype").get< std::shared_ptr< Datatype > >();
    *dtype = d;

    int ndims = H5Sget_simple_extent_ndims(dataset_space);
    std::vector< hsize_t > dims(ndims, 0);
    std::vector< hsize_t > maxdims(ndims, 0);

    H5Sget_simple_extent_dims(dataset_space,
                              dims.data(),
                              maxdims.data());
    Extent e;
    for( auto const& val : dims )
        e.push_back(val);
    auto extent = parameters.at("extent").get< std::shared_ptr< Extent > >();
    *extent = e;

    herr_t status;
    status = H5Sclose(dataset_space);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 dataset space during dataset opening");
    status = H5Tclose(dataset_type);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 dataset type during dataset opening");
    status = H5Dclose(dataset_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 dataset during dataset opening");
    status = H5Gclose(node_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 group during dataset opening");

    writable->written = true;
    writable->abstractFilePosition = std::make_shared< HDF5FilePosition >(name);

    m_fileIDs[writable] = res->second;
}

void
HDF5IOHandlerImpl::deleteFile(Writable* writable,
                              ArgumentMap const& parameters)
{
    if( m_handler->accessType == AccessType::READ_ONLY )
        throw std::runtime_error("Deleting a file opened as read only is not possible.");

    if( writable->written )
    {
        hid_t file_id = m_fileIDs[writable];
        herr_t status = H5Fclose(file_id);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 file during file deletion");

        std::string name = m_handler->directory + parameters.at("name").get< std::string >();
        if( !auxiliary::ends_with(name, ".h5") )
            name += ".h5";

        using namespace boost::filesystem;
        path file(name);
        if( !exists(file) )
            throw std::runtime_error("File does not exist: " + name);

        remove(file);

        writable->written = false;
        writable->abstractFilePosition.reset();

        m_openFileIDs.erase(file_id);
        m_fileIDs.erase(writable);
    }
}

void
HDF5IOHandlerImpl::deletePath(Writable* writable,
                              ArgumentMap const& parameters)
{
    if( m_handler->accessType == AccessType::READ_ONLY )
        throw std::runtime_error("Deleting a path in a file opened as read only is not possible.");

    if( writable->written )
    {
        /* Sanitize path */
        std::string path = parameters.at("path").get< std::string >();
        if( auxiliary::starts_with(path, "/") )
            path = auxiliary::replace_first(path, "/", "");
        if( !auxiliary::ends_with(path, "/") )
            path += '/';

        /* Open H5Object to delete in
         * Ugly hack: H5Ldelete can't delete "."
         *            Work around this by deleting from the parent
         */
        auto res = m_fileIDs.find(writable);
        if( res == m_fileIDs.end() )
            res = m_fileIDs.find(writable->parent);
        hid_t node_id = H5Gopen(res->second,
                                concrete_h5_file_position(writable->parent).c_str(),
                                H5P_DEFAULT);
        ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during path deletion");

        path += static_cast< HDF5FilePosition* >(writable->abstractFilePosition.get())->location;
        herr_t status = H5Ldelete(node_id,
                                  path.c_str(),
                                  H5P_DEFAULT);
        ASSERT(status == 0, "Internal error: Failed to delete HDF5 group");

        status = H5Gclose(node_id);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 group during path deletion");

        writable->written = false;
        writable->abstractFilePosition.reset();

        m_fileIDs.erase(writable);
    }
}

void
HDF5IOHandlerImpl::deleteDataset(Writable* writable,
                                 ArgumentMap const& parameters)
{
    if( m_handler->accessType == AccessType::READ_ONLY )
        throw std::runtime_error("Deleting a path in a file opened as read only is not possible.");

    if( writable->written )
    {
        /* Sanitize name */
        std::string name = parameters.at("name").get< std::string >();
        if( auxiliary::starts_with(name, "/") )
            name = auxiliary::replace_first(name, "/", "");
        if( !auxiliary::ends_with(name, "/") )
            name += '/';

        /* Open H5Object to delete in
         * Ugly hack: H5Ldelete can't delete "."
         *            Work around this by deleting from the parent
         */
        auto res = m_fileIDs.find(writable);
        if( res == m_fileIDs.end() )
            res = m_fileIDs.find(writable->parent);
        hid_t node_id = H5Gopen(res->second,
                                concrete_h5_file_position(writable->parent).c_str(),
                                H5P_DEFAULT);
        ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during dataset deletion");

        name += static_cast< HDF5FilePosition* >(writable->abstractFilePosition.get())->location;
        herr_t status = H5Ldelete(node_id,
                                  name.c_str(),
                                  H5P_DEFAULT);
        ASSERT(status == 0, "Internal error: Failed to delete HDF5 group");

        status = H5Gclose(node_id);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 group during dataset deletion");

        writable->written = false;
        writable->abstractFilePosition.reset();

        m_fileIDs.erase(writable);
    }
}

void
HDF5IOHandlerImpl::deleteAttribute(Writable* writable,
                                   ArgumentMap const& parameters)
{
    if( m_handler->accessType == AccessType::READ_ONLY )
        throw std::runtime_error("Deleting an attribute in a file opened as read only is not possible.");

    if( writable->written )
    {
        std::string name = parameters.at("name").get< std::string >();

        /* Open H5Object to delete in */
        auto res = m_fileIDs.find(writable);
        if( res == m_fileIDs.end() )
            res = m_fileIDs.find(writable->parent);
        hid_t node_id = H5Oopen(res->second,
                                concrete_h5_file_position(writable).c_str(),
                                H5P_DEFAULT);
        ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during attribute deletion");

        herr_t status = H5Adelete(node_id,
                                  name.c_str());
        ASSERT(status == 0, "Internal error: Failed to delete HDF5 attribute");

        status = H5Oclose(node_id);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 group during attribute deletion");
    }
}

void
HDF5IOHandlerImpl::writeDataset(Writable* writable,
                                ArgumentMap const& parameters)
{
    auto res = m_fileIDs.find(writable);
    if( res == m_fileIDs.end() )
        res = m_fileIDs.find(writable->parent);

    hid_t dataset_id, filespace, memspace;
    herr_t status;
    dataset_id = H5Dopen(res->second,
                         concrete_h5_file_position(writable).c_str(),
                         H5P_DEFAULT);
    ASSERT(dataset_id >= 0, "Internal error: Failed to open HDF5 dataset during dataset write");

    std::vector< hsize_t > start;
    for( auto const& val : parameters.at("offset").get< Offset >() )
        start.push_back(static_cast< hsize_t >(val));
    std::vector< hsize_t > stride(start.size(), 1); /* contiguous region */
    std::vector< hsize_t > count(start.size(), 1); /* single region */
    std::vector< hsize_t > block;
    for( auto const& val : parameters.at("extent").get< Extent >() )
        block.push_back(static_cast< hsize_t >(val));
    memspace = H5Screate_simple(block.size(), block.data(), nullptr);
    filespace = H5Dget_space(dataset_id);
    status = H5Sselect_hyperslab(filespace,
                                 H5S_SELECT_SET,
                                 start.data(),
                                 stride.data(),
                                 count.data(),
                                 block.data());
    ASSERT(status == 0, "Internal error: Failed to select hyperslab during dataset write");

    std::shared_ptr< void > data = parameters.at("data").get< std::shared_ptr< void > >();

    Attribute a(0);
    a.dtype = parameters.at("dtype").get< Datatype >();
    hid_t dataType = getH5DataType(a);
    ASSERT(dataType >= 0, "Internal error: Failed to get HDF5 datatype during dataset write");
    switch( a.dtype )
    {
        using DT = Datatype;
        case DT::DOUBLE:
        case DT::FLOAT:
        case DT::INT16:
        case DT::INT32:
        case DT::INT64:
        case DT::UINT16:
        case DT::UINT32:
        case DT::UINT64:
        case DT::CHAR:
        case DT::UCHAR:
        case DT::BOOL:
            status = H5Dwrite(dataset_id,
                              dataType,
                              memspace,
                              filespace,
                              m_datasetTransferProperty,
                              data.get());
            ASSERT(status == 0, "Internal error: Failed to write dataset " + concrete_h5_file_position(writable));
            break;
        case DT::UNDEFINED:
            throw std::runtime_error("Unknown Attribute datatype");
        case DT::DATATYPE:
            throw std::runtime_error("Meta-Datatype leaked into IO");
        default:
            throw std::runtime_error("Datatype not implemented in HDF5 IO");
    }
    status = H5Tclose(dataType);
    ASSERT(status == 0, "Internal error: Failed to close dataset datatype during dataset write");
    status = H5Sclose(filespace);
    ASSERT(status == 0, "Internal error: Failed to close dataset file space during dataset write");
    status = H5Sclose(memspace);
    ASSERT(status == 0, "Internal error: Failed to close dataset memory space during dataset write");
    status = H5Dclose(dataset_id);
    ASSERT(status == 0, "Internal error: Failed to close dataset " + concrete_h5_file_position(writable) + " during dataset write");

    m_fileIDs[writable] = res->second;
}

void
HDF5IOHandlerImpl::writeAttribute(Writable* writable,
                                          ArgumentMap const& parameters)
{
    auto res = m_fileIDs.find(writable);
    if( res == m_fileIDs.end() )
        res = m_fileIDs.find(writable->parent);
    hid_t node_id, attribute_id;
    node_id = H5Oopen(res->second,
                      concrete_h5_file_position(writable).c_str(),
                      H5P_DEFAULT);
    ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 object during attribute write");
    std::string name = parameters.at("name").get< std::string >();
    Attribute const att(parameters.at("attribute").get< Attribute::resource >());
    Datatype dtype = parameters.at("dtype").get< Datatype >();
    herr_t status;
    hid_t dataType;
    if( dtype == Datatype::BOOL )
        dataType = m_H5T_BOOL_ENUM;
    else
        dataType = getH5DataType(att);
    ASSERT(dataType >= 0, "Internal error: Failed to get HDF5 datatype during attribute write");
    if( H5Aexists(node_id, name.c_str()) == 0 )
    {
        hid_t dataspace = getH5DataSpace(att);
        ASSERT(dataspace >= 0, "Internal error: Failed to get HDF5 dataspace during attribute write");
        attribute_id = H5Acreate(node_id,
                                 name.c_str(),
                                 dataType,
                                 dataspace,
                                 H5P_DEFAULT,
                                 H5P_DEFAULT);
        ASSERT(node_id >= 0, "Internal error: Failed to create HDF5 attribute during attribute write");
        status = H5Sclose(dataspace);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 dataspace during attribute write");
    } else
    {
        attribute_id = H5Aopen(node_id,
                               name.c_str(),
                               H5P_DEFAULT);
        ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 attribute during attribute write");
    }

    using DT = Datatype;
    switch( dtype )
    {
        case DT::CHAR:
        {
            char c = att.get< char >();
            status = H5Awrite(attribute_id, dataType, &c);
            break;
        }
        case DT::UCHAR:
        {
            unsigned char u = att.get< unsigned char >();
            status = H5Awrite(attribute_id, dataType, &u);
            break;
        }
        case DT::INT16:
        {
            int16_t i = att.get< int16_t >();
            status = H5Awrite(attribute_id, dataType, &i);
            break;
        }
        case DT::INT32:
        {
            int32_t i = att.get< int32_t >();
            status = H5Awrite(attribute_id, dataType, &i);
            break;
        }
        case DT::INT64:
        {
            int64_t i = att.get< int64_t >();
            status = H5Awrite(attribute_id, dataType, &i);
            break;
        }
        case DT::UINT16:
        {
            uint16_t u = att.get< uint16_t >();
            status = H5Awrite(attribute_id, dataType, &u);
            break;
        }
        case DT::UINT32:
        {
            uint32_t u = att.get< uint32_t >();
            status = H5Awrite(attribute_id, dataType, &u);
            break;
        }
        case DT::UINT64:
        {
            uint64_t u = att.get< uint64_t >();
            status = H5Awrite(attribute_id, dataType, &u);
            break;
        }
        case DT::FLOAT:
        {
            float f = att.get< float >();
            status = H5Awrite(attribute_id, dataType, &f);
            break;
        }
        case DT::DOUBLE:
        {
            double d = att.get< double >();
            status = H5Awrite(attribute_id, dataType, &d);
            break;
        }
        case DT::LONG_DOUBLE:
        {
            long double d = att.get< long double >();
            status = H5Awrite(attribute_id, dataType, &d);
            break;
        }
        case DT::STRING:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::string >().c_str());
            break;
        case DT::VEC_CHAR:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< char > >().data());
            break;
        case DT::VEC_INT16:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< int16_t > >().data());
            break;
        case DT::VEC_INT32:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< int32_t > >().data());
            break;
        case DT::VEC_INT64:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< int64_t > >().data());
            break;
        case DT::VEC_UCHAR:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< unsigned char > >().data());
            break;
        case DT::VEC_UINT16:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< uint16_t > >().data());
            break;
        case DT::VEC_UINT32:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< uint32_t > >().data());
            break;
        case DT::VEC_UINT64:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< uint64_t > >().data());
            break;
        case DT::VEC_FLOAT:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< float > >().data());
            break;
        case DT::VEC_DOUBLE:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< double > >().data());
            break;
        case DT::VEC_LONG_DOUBLE:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::vector< long double > >().data());
            break;
        case DT::VEC_STRING:
        {
            auto vs = att.get< std::vector< std::string > >();
            size_t max_len = 0;
            for( std::string const& s : vs )
                max_len = std::max(max_len, s.size());
            std::unique_ptr< char[] > c_str(new char[max_len * vs.size()]);
            for( size_t i = 0; i < vs.size(); ++i )
                strncpy(c_str.get() + i*max_len, vs[i].c_str(), max_len);
            status = H5Awrite(attribute_id, dataType, c_str.get());
            break;
        }
        case DT::ARR_DBL_7:
            status = H5Awrite(attribute_id,
                              dataType,
                              att.get< std::array< double, 7 > >().data());
            break;
        case DT::BOOL:
        {
            bool b = att.get< bool >();
            status = H5Awrite(attribute_id, dataType, &b);
            break;
        }
        case DT::UNDEFINED:
        case DT::DATATYPE:
            throw std::runtime_error("Unknown Attribute datatype");
        default:
            throw std::runtime_error("Datatype not implemented in HDF5 IO");
    }
    ASSERT(status == 0, "Internal error: Failed to write attribute " + name + " at " + concrete_h5_file_position(writable));

    if( dataType != m_H5T_BOOL_ENUM )
    {
        status = H5Tclose(dataType);
        ASSERT(status == 0, "Internal error: Failed to close HDF5 datatype during Attribute write");
    }

    status = H5Aclose(attribute_id);
    ASSERT(status == 0, "Internal error: Failed to close attribute " + name + " at " + concrete_h5_file_position(writable) + " during attribute write");
    status = H5Oclose(node_id);
    ASSERT(status == 0, "Internal error: Failed to close " + concrete_h5_file_position(writable) + " during attribute write");

    m_fileIDs[writable] = res->second;
}

void
HDF5IOHandlerImpl::readDataset(Writable* writable,
                               std::map< std::string, ParameterArgument > & parameters)
{
    auto res = m_fileIDs.find(writable);
    if( res == m_fileIDs.end() )
        res = m_fileIDs.find(writable->parent);
    hid_t dataset_id, memspace, filespace;
    herr_t status;
    dataset_id = H5Dopen(res->second,
                         concrete_h5_file_position(writable).c_str(),
                         H5P_DEFAULT);
    ASSERT(dataset_id >= 0, "Internal error: Failed to open HDF5 dataset during dataset read");

    std::vector< hsize_t > start;
    for( auto const& val : parameters.at("offset").get< Offset >() )
        start.push_back(static_cast<hsize_t>(val));
    std::vector< hsize_t > stride(start.size(), 1); /* contiguous region */
    std::vector< hsize_t > count(start.size(), 1); /* single region */
    std::vector< hsize_t > block;
    for( auto const& val : parameters.at("extent").get< Extent >() )
        block.push_back(static_cast< hsize_t >(val));
    memspace = H5Screate_simple(block.size(), block.data(), nullptr);
    filespace = H5Dget_space(dataset_id);
    status = H5Sselect_hyperslab(filespace,
                                 H5S_SELECT_SET,
                                 start.data(),
                                 stride.data(),
                                 count.data(),
                                 block.data());
    ASSERT(status == 0, "Internal error: Failed to select hyperslab during dataset read");

    void* data = parameters.at("data").get< void* >();

    Attribute a(0);
    a.dtype = parameters.at("dtype").get< Datatype >();
    switch( a.dtype )
    {
        using DT = Datatype;
        case DT::DOUBLE:
        case DT::FLOAT:
        case DT::INT16:
        case DT::INT32:
        case DT::INT64:
        case DT::UINT16:
        case DT::UINT32:
        case DT::UINT64:
        case DT::CHAR:
        case DT::UCHAR:
        case DT::BOOL:
            break;
        case DT::UNDEFINED:
            throw std::runtime_error("Unknown Attribute datatype");
        case DT::DATATYPE:
            throw std::runtime_error("Meta-Datatype leaked into IO");
        default:
            throw std::runtime_error("Datatype not implemented in HDF5 IO");
    }
    hid_t dataType = getH5DataType(a);
    ASSERT(dataType >= 0, "Internal error: Failed to get HDF5 datatype during dataset read");
    status = H5Dread(dataset_id,
                     dataType,
                     memspace,
                     filespace,
                     m_datasetTransferProperty,
                     data);
    ASSERT(status == 0, "Internal error: Failed to read dataset");

    status = H5Tclose(dataType);
    ASSERT(status == 0, "Internal error: Failed to close dataset datatype during dataset read");
    status = H5Sclose(filespace);
    ASSERT(status == 0, "Internal error: Failed to close dataset file space during dataset read");
    status = H5Sclose(memspace);
    ASSERT(status == 0, "Internal error: Failed to close dataset memory space during dataset read");
    status = H5Dclose(dataset_id);
    ASSERT(status == 0, "Internal error: Failed to close dataset during dataset read");
}

void
HDF5IOHandlerImpl::readAttribute(Writable* writable,
                                 std::map< std::string, ParameterArgument >& parameters)
{
    auto res = m_fileIDs.find(writable);
    if( res == m_fileIDs.end() )
        res = m_fileIDs.find(writable->parent);

    hid_t obj_id, attr_id;
    herr_t status;
    obj_id = H5Oopen(res->second,
                     concrete_h5_file_position(writable).c_str(),
                     H5P_DEFAULT);
    ASSERT(obj_id >= 0, "Internal error: Failed to open HDF5 object during attribute read");
    std::string const & attr_name = parameters.at("name").get< std::string >();
    attr_id = H5Aopen(obj_id,
                      attr_name.c_str(),
                      H5P_DEFAULT);
    ASSERT(attr_id >= 0, "Internal error: Failed to open HDF5 attribute during attribute read");

    hid_t attr_type, attr_space;
    attr_type = H5Aget_type(attr_id);
    attr_space = H5Aget_space(attr_id);

    int ndims = H5Sget_simple_extent_ndims(attr_space);
    std::vector< hsize_t > dims(ndims, 0);
    std::vector< hsize_t > maxdims(ndims, 0);

    status = H5Sget_simple_extent_dims(attr_space,
                                       dims.data(),
                                       maxdims.data());
    ASSERT(status == ndims, "Internal error: Failed to get dimensions during attribute read");

    H5S_class_t attr_class = H5Sget_simple_extent_type(attr_space);
    Attribute a(0);
    if( attr_class == H5S_SCALAR || (attr_class == H5S_SIMPLE && ndims == 1 && dims[0] == 1) )
    {
        if( H5Tequal(attr_type, H5T_NATIVE_CHAR) )
        {
            char c;
            status = H5Aread(attr_id,
                             attr_type,
                             &c);
            a = Attribute(c);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UCHAR) )
        {
            unsigned char u;
            status = H5Aread(attr_id,
                             attr_type,
                             &u);
            a = Attribute(u);
        } else if( H5Tequal(attr_type, H5T_NATIVE_INT16) )
        {
            int16_t i;
            status = H5Aread(attr_id,
                             attr_type,
                             &i);
            a = Attribute(i);
        } else if( H5Tequal(attr_type, H5T_NATIVE_INT32) )
        {
            int32_t i;
            status = H5Aread(attr_id,
                             attr_type,
                             &i);
            a = Attribute(i);
        } else if( H5Tequal(attr_type, H5T_NATIVE_INT64) )
        {
            int64_t i;
            status = H5Aread(attr_id,
                             attr_type,
                             &i);
            a = Attribute(i);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UINT16) )
        {
            uint16_t u;
            status = H5Aread(attr_id,
                             attr_type,
                             &u);
            a = Attribute(u);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UINT32) )
        {
            uint32_t u;
            status = H5Aread(attr_id,
                             attr_type,
                             &u);
            a = Attribute(u);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UINT64) )
        {
            uint64_t u;
            status = H5Aread(attr_id,
                             attr_type,
                             &u);
            a = Attribute(u);
        } else if( H5Tequal(attr_type, H5T_NATIVE_FLOAT) )
        {
            float f;
            status = H5Aread(attr_id,
                             attr_type,
                             &f);
            a = Attribute(f);
        } else if( H5Tequal(attr_type, H5T_NATIVE_DOUBLE) )
        {
            double d;
            status = H5Aread(attr_id,
                             attr_type,
                             &d);
            a = Attribute(d);
        } else if( H5Tequal(attr_type, H5T_NATIVE_LDOUBLE) )
        {
            long double l;
            status = H5Aread(attr_id,
                             attr_type,
                             &l);
            a = Attribute(l);
        } else if( H5Tget_class(attr_type) == H5T_STRING )
        {
            if( H5Tis_variable_str(attr_type) )
            {
                char* c = nullptr;
                status = H5Aread(attr_id,
                                 attr_type,
                                 c);
                a = Attribute(auxiliary::strip(std::string(c), {'\0'}));
                status = H5Dvlen_reclaim(attr_type,
                                         attr_space,
                                         H5P_DEFAULT,
                                         c);
            } else
            {
                hsize_t size = H5Tget_size(attr_type);
                std::vector< char > vc(size);
                status = H5Aread(attr_id,
                                 attr_type,
                                 vc.data());
                a = Attribute(auxiliary::strip(std::string(vc.data(), size), {'\0'}));
            }
        } else if( H5Tget_class(attr_type) == H5T_ENUM )
        {
            bool attrIsBool = false;
            if( H5Tget_nmembers(attr_type) == 2 )
            {
                char* m0 = H5Tget_member_name(attr_type, 0);
                char* m1 = H5Tget_member_name(attr_type, 1);
                if( (strcmp("TRUE" , m0) == 0) && (strcmp("FALSE", m1) == 0) )
                    attrIsBool = true;
                H5free_memory(m1);
                H5free_memory(m0);
            }

            if( attrIsBool )
            {
                int8_t enumVal;
                status = H5Aread(attr_id,
                                 attr_type,
                                 &enumVal);
                a = Attribute(static_cast< bool >(enumVal));
            } else
                throw unsupported_data_error("Unsupported attribute enumeration");
        } else if( H5Tget_class(attr_type) == H5T_COMPOUND )
            throw unsupported_data_error("Compound attribute type not supported");
        else
            throw std::runtime_error("Unsupported scalar attribute type");
    } else if( attr_class == H5S_SIMPLE )
    {
        if( ndims != 1 )
            throw std::runtime_error("Unsupported attribute (array with ndims != 1)");

        if( H5Tequal(attr_type, H5T_NATIVE_CHAR) )
        {
            std::vector< char > vc(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vc.data());
            a = Attribute(vc);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UCHAR) )
        {
            std::vector< unsigned char > vu(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vu.data());
            a = Attribute(vu);
        } else if( H5Tequal(attr_type, H5T_NATIVE_INT16) )
        {
            std::vector< int16_t > vint16(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vint16.data());
            a = Attribute(vint16);
        } else if( H5Tequal(attr_type, H5T_NATIVE_INT32) )
        {
            std::vector< int32_t > vint32(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vint32.data());
            a = Attribute(vint32);
        } else if( H5Tequal(attr_type, H5T_NATIVE_INT64) )
        {
            std::vector< int64_t > vint64(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vint64.data());
            a = Attribute(vint64);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UINT16) )
        {
            std::vector< uint16_t > vuint16(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vuint16.data());
            a = Attribute(vuint16);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UINT32) )
        {
            std::vector< uint32_t > vuint32(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vuint32.data());
            a = Attribute(vuint32);
        } else if( H5Tequal(attr_type, H5T_NATIVE_UINT64) )
        {
            std::vector< uint64_t > vuint64(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vuint64.data());
            a = Attribute(vuint64);
        } else if( H5Tequal(attr_type, H5T_NATIVE_FLOAT) )
        {
            std::vector< float > vf(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vf.data());
            a = Attribute(vf);
        } else if( H5Tequal(attr_type, H5T_NATIVE_DOUBLE) )
        {
            if( dims[0] == 7 && attr_name == "unitDimension" )
            {
                std::array< double, 7 > ad;
                status = H5Aread(attr_id,
                                 attr_type,
                                 &ad);
                a = Attribute(ad);
            } else
            {
                std::vector< double > vd(dims[0], 0);
                status = H5Aread(attr_id,
                                 attr_type,
                                 vd.data());
                a = Attribute(vd);
            }
        } else if( H5Tequal(attr_type, H5T_NATIVE_LDOUBLE) )
        {
            std::vector< long double > vld(dims[0], 0);
            status = H5Aread(attr_id,
                             attr_type,
                             vld.data());
            a = Attribute(vld);
        } else if( H5Tget_class(attr_type) == H5T_STRING )
        {
            std::vector< std::string > vs;
            if( H5Tis_variable_str(attr_type) )
            {
                std::vector< char * > vc(dims[0]);
                status = H5Aread(attr_id,
                                 attr_type,
                                 vc.data());
                for( auto const& val : vc )
                    vs.push_back(auxiliary::strip(std::string(val), {'\0'}));
                status = H5Dvlen_reclaim(attr_type,
                                         attr_space,
                                         H5P_DEFAULT,
                                         vc.data());
            } else
            {
                size_t length = H5Tget_size(attr_type);
                std::vector< char > c(dims[0] * length);
                status = H5Aread(attr_id,
                                 attr_type,
                                 c.data());
                for( hsize_t i = 0; i < dims[0]; ++i )
                    vs.push_back(auxiliary::strip(std::string(c.data() + i*length, length), {'\0'}));
            }
            a = Attribute(vs);
        } else
            throw std::runtime_error("Unsupported simple attribute type");
    } else
        throw std::runtime_error("Unsupported attribute class");
    ASSERT(status == 0, "Internal error: Failed to read attribute " + attr_name + " at " + concrete_h5_file_position(writable));

    status = H5Tclose(attr_type);
    ASSERT(status == 0, "Internal error: Failed to close attribute datatype during attribute read");
    status = H5Sclose(attr_space);
    ASSERT(status == 0, "Internal error: Failed to close attribute file space during attribute read");

    auto dtype = parameters.at("dtype").get< std::shared_ptr< Datatype > >();
    *dtype = a.dtype;
    auto resource = parameters.at("resource").get< std::shared_ptr< Attribute::resource > >();
    *resource = a.getResource();

    status = H5Aclose(attr_id);
    ASSERT(status == 0, "Internal error: Failed to close attribute " + attr_name + " at " + concrete_h5_file_position(writable) + " during attribute read");
    status = H5Oclose(obj_id);
    ASSERT(status == 0, "Internal error: Failed to close " + concrete_h5_file_position(writable) + " during attribute read");
}

void
HDF5IOHandlerImpl::listPaths(Writable* writable,
                             std::map< std::string, ParameterArgument > & parameters)
{
    auto res = m_fileIDs.find(writable);
    if( res == m_fileIDs.end() )
        res = m_fileIDs.find(writable->parent);
    hid_t node_id = H5Gopen(res->second,
                            concrete_h5_file_position(writable).c_str(),
                            H5P_DEFAULT);
    ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during path listing");

    H5G_info_t group_info;
    herr_t status = H5Gget_info(node_id, &group_info);
    ASSERT(status == 0, "Internal error: Failed to get HDF5 group info for " + concrete_h5_file_position(writable) + " during path listing");

    auto paths = parameters.at("paths").get< std::shared_ptr< std::vector< std::string > > >();
    for( hsize_t i = 0; i < group_info.nlinks; ++i )
    {
        if( H5G_GROUP == H5Gget_objtype_by_idx(node_id, i) )
        {
            ssize_t name_length = H5Gget_objname_by_idx(node_id, i, nullptr, 0);
            std::vector< char > name(name_length+1);
            H5Gget_objname_by_idx(node_id, i, name.data(), name_length+1);
            paths->push_back(std::string(name.data(), name_length));
        }
    }

    status = H5Gclose(node_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 group " + concrete_h5_file_position(writable) + " during path listing");
}

void
HDF5IOHandlerImpl::listDatasets(Writable* writable,
                                std::map< std::string, ParameterArgument >& parameters)
{
    auto res = m_fileIDs.find(writable);
    if( res == m_fileIDs.end() )
        res = m_fileIDs.find(writable->parent);
    hid_t node_id = H5Gopen(res->second,
                            concrete_h5_file_position(writable).c_str(),
                            H5P_DEFAULT);
    ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during dataset listing");

    H5G_info_t group_info;
    herr_t status = H5Gget_info(node_id, &group_info);
    ASSERT(status == 0, "Internal error: Failed to get HDF5 group info for " + concrete_h5_file_position(writable) + " during dataset listing");

    auto datasets = parameters.at("datasets").get< std::shared_ptr< std::vector< std::string > > >();
    for( hsize_t i = 0; i < group_info.nlinks; ++i )
    {
        if( H5G_DATASET == H5Gget_objtype_by_idx(node_id, i) )
        {
            ssize_t name_length = H5Gget_objname_by_idx(node_id, i, nullptr, 0);
            std::vector< char > name(name_length+1);
            H5Gget_objname_by_idx(node_id, i, name.data(), name_length+1);
            datasets->push_back(std::string(name.data(), name_length));
        }
    }

    status = H5Gclose(node_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 group " + concrete_h5_file_position(writable) + " during dataset listing");
}

void HDF5IOHandlerImpl::listAttributes(Writable* writable,
                                       std::map< std::string, ParameterArgument >& parameters)
{
    auto res = m_fileIDs.find(writable);
    if( res == m_fileIDs.end() )
        res = m_fileIDs.find(writable->parent);
    hid_t node_id;
    node_id = H5Oopen(res->second,
                      concrete_h5_file_position(writable).c_str(),
                      H5P_DEFAULT);
    ASSERT(node_id >= 0, "Internal error: Failed to open HDF5 group during attribute listing");

    H5O_info_t object_info;
    herr_t status;
    status = H5Oget_info(node_id, &object_info);
    ASSERT(status == 0, "Internal error: Failed to get HDF5 object info for " + concrete_h5_file_position(writable) + " during attribute listing");

    auto strings = parameters.at("attributes").get< std::shared_ptr< std::vector< std::string > > >();
    for( hsize_t i = 0; i < object_info.num_attrs; ++i )
    {
        ssize_t name_length = H5Aget_name_by_idx(node_id,
                                                 ".",
                                                 H5_INDEX_CRT_ORDER,
                                                 H5_ITER_INC,
                                                 i,
                                                 nullptr,
                                                 0,
                                                 H5P_DEFAULT);
        std::vector< char > name(name_length+1);
        H5Aget_name_by_idx(node_id,
                           ".",
                           H5_INDEX_CRT_ORDER,
                           H5_ITER_INC,
                           i,
                           name.data(),
                           name_length+1,
                           H5P_DEFAULT);
        strings->push_back(std::string(name.data(), name_length));
    }

    status = H5Oclose(node_id);
    ASSERT(status == 0, "Internal error: Failed to close HDF5 object during attribute listing");
}
#endif

#if defined(openPMD_HAVE_HDF5)
HDF5IOHandler::HDF5IOHandler(std::string const& path, AccessType at)
        : AbstractIOHandler(path, at),
          m_impl{new HDF5IOHandlerImpl(this)}
{ }

HDF5IOHandler::~HDF5IOHandler()
{ }

std::future< void >
HDF5IOHandler::flush()
{
    return m_impl->flush();
}
#else
HDF5IOHandler::HDF5IOHandler(std::string const& path, AccessType at)
        : AbstractIOHandler(path, at)
{
    throw std::runtime_error("openPMD-api built without HDF5 support");
}

HDF5IOHandler::~HDF5IOHandler()
{ }

std::future< void >
HDF5IOHandler::flush()
{
    return std::future< void >();
}
#endif
} // openPMD
