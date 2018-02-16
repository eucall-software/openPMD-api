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

#include "Series.hpp"

int main(int argc, char *argv[])
{
  /* The root of any openPMD output spans across all data for all iterations is a 'Series'.
   * Data is either in a single file or spread across multiple files. */
  Series series = Series::create("sample/1_structure.h5");

  /* Every element that structures your file (groups and datasets for example) can be annotated with attributes. */
  series.setComment("This string will show up at the root ('/') of the output with key 'comment'.");

  /* Access to individual positions inside happens hierarchically, according to the openPMD standard.
   * Creation of new elements happens on access inside the tree-like structure.
   * Required attributes are initialized to reasonable defaults for every object. */
  ParticleSpecies &electrons = series.iterations[1].particles["electrons"];

  /* Data to be moved from memory to persistent storage is structured into Records,
   * each holding an unbounded number of RecordComponents.
   * If a Record only contains a single (scalar) component, it is treated slightly different.
   * https://github.com/openPMD/openPMD-standard/blob/latest/STANDARD.md#scalar-vector-and-tensor-records*/
  Record          &mass        = electrons["mass"];
  RecordComponent &mass_scalar = electrons["mass"][RecordComponent::SCALAR];

  Dataset dataset = Dataset(Datatype::DOUBLE, Extent{1});
  mass_scalar.resetDataset(dataset);

  return 0;
}
