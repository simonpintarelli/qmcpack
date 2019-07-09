//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2019 QMCPACK developers.
//
// File developed by: Eric Neuscamman, eneuscamman@berkeley.edu, University of California, Berkeley
//
// File created by: Eric Neuscamman, eneuscamman@berkeley.edu, University of California, Berkeley
//////////////////////////////////////////////////////////////////////////////////////


#include "catch.hpp"

#include "Configuration.h"
#include "Message/Communicate.h"
#include "OhmmsData/Libxml2Doc.h"
#include "QMCWaveFunctions/Jastrow/ShortRangeCuspFunctor.h"

namespace qmcplusplus
{
TEST_CASE("ShortRangeCuspJastrowFunctor", "[wavefunction]")
{
  using RealType = OptimizableFunctorBase::real_type;

  OHMMS::Controller->initialize(0, NULL);
  Communicate* c = OHMMS::Controller;

  // prepare xml input to set up the functor
  const std::string xmltext("<tmp>"
                              "<correlation rcut=\"6\" cusp=\"3\" elementType=\"Li\">"
                                "<var id=\"LiCuspR0\" name=\"R0\" optimize=\"yes\"> 0.0624 </var>"
                                "<coefficients id=\"LiCuspB\" type=\"Array\" optimize=\"yes\">"
                                 " 0.3 0.2 0.4 "
                                "</coefficients>"
                              "</correlation>"
                            "</tmp>");

  // parse the xml input
  Libxml2Document doc;
  const bool xml_parsed_okay = doc.parseFromString(xmltext);
  REQUIRE( xml_parsed_okay == true );

  // get the xml root node pointer
  xmlNodePtr root = doc.getRoot();
  REQUIRE( root != NULL );

  // get the xml pointer to the functor node
  xmlNodePtr child = root->xmlChildrenNode;
  REQUIRE( child != NULL );

  // prepare the Jastrow factor using the xml input
  ShortRangeCuspFunctor<RealType> f;
  f.put(child);
  REQUIRE( f.A             == Approx(3.0   )   );
  REQUIRE( f.R0            == Approx(0.0624)   );
  REQUIRE( f.B.at(0)       == Approx(0.3   )   );
  REQUIRE( f.B.at(1)       == Approx(0.2   )   );
  REQUIRE( f.B.at(2)       == Approx(0.4   )   );
  REQUIRE( f.Opt_A         == false            );
  REQUIRE( f.Opt_R0        == true             );
  REQUIRE( f.Opt_B         == true             );
  REQUIRE( f.ID_A          == "string_not_set" );
  REQUIRE( f.ID_R0         == "LiCuspR0"       );
  REQUIRE( f.ID_B          == "LiCuspB"        );
  REQUIRE( f.cutoff_radius == Approx(6.0)      );

  // evaluate a couple ways at a given radius
  RealType r   = 0.04;
  RealType val = f.evaluate(r);
  RealType dudr   = 10000000.0;  // initialize to a wrong value
  RealType d2udr2 = 10000000.0;  // initialize to a wrong value
  RealType val_2 = f.evaluate(r, dudr, d2udr2);

  REQUIRE( val   == Approx(-0.1970331287).epsilon(0.0001) );
  REQUIRE( val_2 == Approx(-0.1970331287).epsilon(0.0001) );
  REQUIRE( val   == Approx(val_2) );

  // Finite difference to verify the spatial derivatives
  const RealType h     = 0.0001;
  RealType r_plus_h    = r + h;
  RealType r_minus_h   = r - h;
  RealType val_plus_h  = f.evaluate(r_plus_h);
  RealType val_minus_h = f.evaluate(r_minus_h);
  RealType approx_dudr = (val_plus_h - val_minus_h) / (2 * h);
  RealType approx_d2udr2 = (val_plus_h + val_minus_h - 2 * val) / (h * h);
  REQUIRE( dudr   == Approx(approx_dudr).epsilon(h)   );
  REQUIRE( d2udr2 == Approx(approx_d2udr2).epsilon(h) );

  // currently the d3udr3_3 function is not implemented
  //RealType dudr_3;
  //RealType d2udr2_3;
  //RealType d3udr3_3;
  //RealType val_3 = f.evaluate(r, dudr_3, d2udr2_3, d3udr3_3);
  //REQUIRE(val == Approx(val_3));
  //REQUIRE(dudr == Approx(dudr_3));
  //REQUIRE(d2udr2 == Approx(d2udr2_3));

  // Now let's do finite difference checks for the parameter derivatives

  // Adjust this based on the number of variational parameters
  const int nparam = 4;

  // Outer vector is over parameters
  // Inner (TinyVector) is the parameter derivative of the value, first, and second derivatives.
  std::vector<TinyVector<RealType, 3>> param_derivs(nparam);

  f.evaluateDerivatives(r, param_derivs);

  optimize::VariableSet var_param;
  f.checkInVariables(var_param);
  var_param.resetIndex();
  REQUIRE(var_param.size_of_active() == nparam);

  for (int i = 0; i < nparam; i++)
  {
    const std::string var_name = var_param.name(i);
    const RealType old_param = std::real(var_param[var_name]);
    //std::cout << "checking parameter " << var_name << std::endl;

    RealType dudr_h   = 10000000.0;  // initialize to a wrong value
    RealType d2udr2_h = 10000000.0;  // initialize to a wrong value
    var_param[var_name] = old_param + h;
    f.resetParameters(var_param);
    RealType val_h = f.evaluate(r, dudr_h, d2udr2_h);

    RealType dudr_m   = 20000000.0;  // initialize to a wrong value
    RealType d2udr2_m = 20000000.0;  // initialize to a wrong value
    var_param[var_name] = old_param - h;
    f.resetParameters(var_param);
    RealType val_m = f.evaluate(r, dudr_m, d2udr2_m);

    const RealType val_dp = ( val_h - val_m ) / ( 2.0 * h );
    REQUIRE( val_dp == Approx(param_derivs[i][0]).epsilon(h) );

    const RealType dudr_dp = ( dudr_h - dudr_m ) / ( 2.0 * h );
    REQUIRE( dudr_dp == Approx(param_derivs[i][1]).epsilon(h) );

    const RealType d2udr2_dp = ( d2udr2_h - d2udr2_m ) / ( 2.0 * h );
    REQUIRE( d2udr2_dp == Approx(param_derivs[i][2]).epsilon(h) );

    var_param[var_name] = old_param;
    f.resetParameters(var_param);
  }

  // Could do finite differences to verify the parameter derivatives
}
} // namespace qmcplusplus
