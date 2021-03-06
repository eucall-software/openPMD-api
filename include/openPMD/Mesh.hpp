#pragma once

#include "openPMD/backend/Attributable.hpp"
#include "openPMD/backend/BaseRecord.hpp"
#include "openPMD/backend/MeshRecordComponent.hpp"

#include <array>
#include <vector>
#include <string>


namespace openPMD
{
/** @brief Container for N-dimensional, homogenous Records.
 *
 * @see https://github.com/openPMD/openPMD-standard/blob/latest/STANDARD.md#mesh-based-records
 */
class Mesh : public BaseRecord< MeshRecordComponent >
{
    friend class Container< Mesh >;
    friend class Iteration;

public:
    Mesh(Mesh const&) = default;

    /** @brief Enumerated datatype for the geometry of the mesh.
     *
     * @note If the default values do not suit your application, you can set arbitrary
     *       Geometry with MeshRecordComponent::setAttribute("geometry", VALUE).
     *       Note that this might break openPMD compliance and tool support.
     */
    enum class Geometry
    {
        cartesian,
        thetaMode,
        cylindrical,
        spherical
    };  //Geometry

    /** @brief Enumerated datatype for the memory layout of N-dimensional data.
     */
    enum class DataOrder : char
    {
        C = 'C',
        F = 'F'
    };  //DataOrder

    /**
     * @return String representing the geometry of the mesh of the mesh record.
     */
    Geometry geometry() const;
    /** Set the geometry of the mesh of the mesh record.
     *
     * @param   geometry    geometry of the mesh of the mesh record.
     * @return  Reference to modified mesh.
     */
    Mesh& setGeometry(Geometry geometry);

    /**
     * @throw   no_such_attribute_error If Mesh::geometry is not Mesh::Geometry::thetaMode.
     * @return  String representing additional parameters for the geometry, separated by a @code ; @endcode.
     */
    std::string geometryParameters() const;
    /** Set additional parameters for the geometry, separated by a @code ; @endcode.
     *
     * @note    Seperation constraint is not verified by API.
     * @param   geometryParameters  additional parameters for the geometry, separated by a @code ; @endcode.
     * @return  Reference to modified mesh.
     */
    Mesh& setGeometryParameters(std::string const& geometryParameters);

    /**
     * @return  Memory layout of N-dimensional data.
     */
    DataOrder dataOrder() const;
    /** Set the memory layout of N-dimensional data.
     *
     * @param   dataOrder   memory layout of N-dimensional data.
     * @return  Refernce to modified mesh.
     */
    Mesh& setDataOrder(DataOrder dataOrder);

    /**
     * @return  Ordering of the labels for the Mesh::geometry of the mesh.
     */
    std::vector< std::string > axisLabels() const;
    /** Set the ordering of the labels for the Mesh::geometry of the mesh.
     *
     * @note    Dimensionality constraint is not verified by API.
     * @param   axisLabels  vector containing N (string) elements, where N is the number of dimensions in the simulation.
     * @return  Reference to modified mesh.
     */
    Mesh& setAxisLabels(std::vector< std::string > axisLabels);

    /**
     * @tparam  T   Floating point type of user-selected precision (e.g. float, double).
     * @return  vector of T representing the spacing of the grid points along each dimension (in the units of the simulation).
     */
    template< typename T >
    std::vector< T > gridSpacing() const;
    /** Set the spacing of the grid points along each dimension (in the units of the simulation).
     *
     * @note    Dimensionality constraint is not verified by API.
     * @tparam  T   Floating point type of user-selected precision (e.g. float, double).
     * @param   gridSpacing     vector containing N (T) elements, where N is the number of dimensions in the simulation.
     * @return  Reference to modified mesh.
     */
    template< typename T >
    Mesh& setGridSpacing(std::vector< T > gridSpacing);

    /**
     * @return  Vector of (double) representing the start of the current domain of the simulation (position of the beginning of the first cell) in simulation units.
     */
    std::vector< double > gridGlobalOffset() const;
    /** Set the start of the current domain of the simulation (position of the beginning of the first cell) in simulation units.
     *
     * @note    Dimensionality constraint is not verified by API.
     * @param   gridGlobalOffset    vector containing N (double) elements, where N is the number of dimensions in the simulation.
     * @return  Reference to modified mesh.
     */
    Mesh& setGridGlobalOffset(std::vector< double > gridGlobalOffset);

    /**
     * @return  Unit-conversion factor to multiply each value in Mesh::gridSpacing and Mesh::gridGlobalOffset, in order to convert from simulation units to SI units.
     */
    double gridUnitSI() const;
    /** Set the unit-conversion factor to multiply each value in Mesh::gridSpacing and Mesh::gridGlobalOffset, in order to convert from simulation units to SI units.
     *
     * @param   gridUnitSI  unit-conversion factor to multiply each value in Mesh::gridSpacing and Mesh::gridGlobalOffset, in order to convert from simulation units to SI units.
     * @return  Reference to modified mesh.
     */
    Mesh& setGridUnitSI(double gridUnitSI);

    /** Set the powers of the 7 base measures characterizing the record's unit in SI.
     *
     * @param   unitDimension   map containing pairs of (UnitDimension, dobule) that represent the power of the particular base.
     * @return  Refence to modified mesh.
     */
    Mesh& setUnitDimension(std::map< UnitDimension, double > const& unitDimension);

    /**
     * @tparam  T   Floating point type of user-selected precision (e.g. float, double).
     * @return  Offset between the time at which this record is defined and the Iteration::time attribute of the Series::basePath level.
     */
    template< typename T >
    T timeOffset() const;
    /** Set the offset between the time at which this record is defined and the Iteration::time attribute of the Series::basePath level.
     *
     * @note    This should be written in the same unit system as Iteration::time.
     * @tparam  T   Floating point type of user-selected precision (e.g. float, double).
     * @param   timeOffset  Offset between the time at which this record is defined and the Iteration::time attribute of the Series::basePath level.
     * @return  Reference to modified mesh.
     */
    template< typename T >
    Mesh& setTimeOffset(T timeOffset);

private:
    Mesh();

    void flush(std::string const&) override;
    void read() override;
}; // Mesh

template< typename T >
inline std::vector< T >
Mesh::gridSpacing() const
{ return readVectorFloatingpoint< T >("gridSpacing"); }

template< typename T >
inline T
Mesh::timeOffset() const
{ return readFloatingpoint< T >("timeOffset"); }
} // openPMD

namespace std
{
    ostream&
    operator<<(ostream&, openPMD::Mesh::Geometry);

    std::ostream&
    operator<<(ostream&, openPMD::Mesh::DataOrder);
} // std
