/* Copyright 2011-2014 Kyle Michel, Logan Ward, Christopher Wolverton
 *
 * Contact: Kyle Michel (kylemichel@gmail.com)
 *			Logan Ward (LoganWard2012@u.northwestern.edu)
 *
 *
 * This file is part of Mint.
 *
 * Mint is free software: you can redistribute it and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * Mint is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with Mint.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "multi.h"
#include "diffraction.h"
#include "language.h"
#include "output.h"
#include "random.h"
#include "launcher.h"
#include "timer.h"
#include "dlib/numerical_integration.h"
#include <cstdlib>
#include <vector>
#include <deque>
#include <algorithm>
#include <numeric>

/**
 * Define the structure used to generate a diffraction pattern. Also populates 
 * a list of atoms whose positions can be refined, and calculates locations at which
 * diffraction peaks will appear. 
 * 
 * Note: This operation must be run before any sort of diffraction pattern calculation
 * is run.
 * @param structure [in] Structure to be used in calculations / refinements
 * @param symmetry [in] Symmetry information of that structure
 */
void CalculatedPattern::defineStructure(ISO& structure, const Symmetry& symmetry) {
    // Clear out any old data
    _symmetry = &symmetry;
    _structure = &structure;

    // Get the atomic parameters 
    setATFParams();
    calculatePeakLocations();
    
    // Store the lattice parameters and angles
    _originalLengths = structure.basis().lengths();
    _originalAngles = structure.basis().angles();

    // Initialize guesses for fitting parameters
    initializeRefinementParameters();
}

/**
 * Check whether user has defined the structure from which to calculate the 
 *  diffraction pattern.
 * @return Whether defineStructure has been run
 */
bool CalculatedPattern::structureIsDefined() {
    return _symmetry != 0;
}

void CalculatedPattern::defineReferencePattern(const Diffraction& reference) {
    // Retrieve parameters of reference pattern
    _method = reference.method();
    _wavelength = reference.wavelength();
    _minTwoTheta = reference.minTwoTheta();
    _maxTwoTheta = reference.maxTwoTheta();
    // If a structure is defined, redefine peak locations
    if (structureIsDefined())
        calculatePeakLocations();
}

/**
 * Make initial guesses for refinement parameters based on the provided structure. 
 * 
 * Currently, this retrieves the internal degrees of freedom from the structure.
 * @param structure [in] Structure used to calculate diffraction pattern
 * @param symmetry [in] Symmetry information of structure
 */
void CalculatedPattern::initializeRefinementParameters() {
    // Set initial guess for B factors to 1.0
    _BFactors.resize(_symmetry->orbits().length());
    fill(_BFactors.begin(), _BFactors.end(), 0.5);
    // Note: Internal parameters are kept in _symmetry
}

/**
 * Determine whether a specific parameter is in the set of parameters to be 
 *  refined.
 * @param parameter Parameter in question
 * @param toRefine Set of parameters that will be refined
 * @return Whether this parameter will be refined
 */
bool CalculatedPattern::willRefine(RefinementParameters parameter,
        std::set<RefinementParameters> toRefine) {
    return toRefine.find(parameter) != toRefine.end();
}

/**
 * Calculate the diffraction pattern of a structure and store in this object. If provided with
 * a reference pattern and <code>fitBfactors</code> is true, will fit B factors to best
 * match the provided pattern.
 * 
 * @param iso [in] Structure from which to calculate diffraction pattern
 * @param symmetry [in] Describes symmetry of structure 
 * @param ref [in] Reference pattern to fit intensities and (if desired) BFactors against. Equals 0 if no pattern supplied
 * @param rietveld [in] Use full-pattern refinement (requires intensity measured a function of angle)
 * @param fitBFactors [in] Whether to fit B factors (if pattern provided)
 * @return R factor (if pattern provided)
 */
double CalculatedPattern::set(ISO& iso, const Symmetry& symmetry, const Diffraction* ref, 
		bool rietveld, bool fitBfactors) {
	_type = PT_CALCULATED;
    // Clear space
    clear();

    // Set this structure as the structure to be refined
    defineStructure(iso, symmetry);

    // Define the reference pattern
    if (ref != 0)
        defineReferencePattern(*ref);

    // Output
    Output::newline();
    Output::print("Calculating peak intensities for the structure");
    Output::increase();

    // If a reference pattern was passed then optimize max intensity and B factors
    double rFactor = 0;
    if (ref) {
        // Output
        Output::newline();
        Output::print("Optimizing against reference pattern");
        Output::increase();

		// Decide what is being refined
		std::set<RefinementParameters> toRefine;
		if (fitBfactors)
			toRefine.insert(RF_BFACTORS);
		
		if (rietveld) {
			rietveldRefinement(*ref, toRefine);
			getRietveldRFactor(*ref, DR_ABS);
			this->_measurementAngles = ref->getMeasurementAngles();
			rFactor = getRietveldRFactor(*ref, DR_ABS);
		} else {
			// Refine the scale factor and B factors
			matchPeaksToReference(*ref);
			refineParameters(ref, toRefine);

			rFactor = getCurrentRFactor(*ref, DR_ABS);
		}

        // Output
        Output::newline();
        Output::print("Optimal R factor: ");
        Output::print(rFactor);
        Output::decrease();
    } else {
		// Calculate the peak intensities using initial guesses
        calculatePeakIntensities();
    }
    
    // Find the maximum intensity
	double maxIntensity = 0.0;
	for (int i=0; i<_reflections.size(); i++) {
		if (_reflections[i].getIntensity() > maxIntensity)
			maxIntensity = _reflections[i].getIntensity();
	}
	
	// If no reference pattern, set optimal scale to make the tallest reflection 1000
	if (! ref) {
		_optimalScale = 1000 / maxIntensity;
	}

    // Print intensities (scaled to 1000)
    Output::newline();
    Output::print("Generated ");
    Output::print(_reflections.size());
    Output::print(" peak");
    if (_reflections.size() != 1)
        Output::print("s");
    Output::increase();
    for (int i = 0; i < _reflections.size(); ++i) {
		double angle = _reflections[i].getAngle();
		double intensity = _reflections[i].getIntensity();
        if (intensity < 1e-6 * maxIntensity) 
            continue;
        Output::newline();
        Output::print("Two-theta and intensity of ");
        Output::print(angle);
        Output::print(" ");
        Output::print(intensity * 1000 / maxIntensity);
        Output::print(" - ");
        Vector3D hkl = _reflections[i].getHKL();
        Output::print(hkl, 0, false);
    }
    Output::decrease();

    // Output
    Output::decrease();

    // Return the R factor
    return rFactor;
}

/**
 * Refines the structure against the entire pattern, not the integrated intensities. 
 * 
 * At the moment, the following parameters will be refined (in the following order):
 * 
 * <ol>
 * <li>Scale factor</li>
 * <li>Background parameters (currently set to five)</li>
 * <li>Angle-independent peak-broadening</li>
 * <li>Coordinates of atoms (if in toRefine)</li>
 * <li>Isotropic thermal, B, factors (if in toRefine)</li>
 * </ol>
 * 
 * See Pecharsky pg. 521 for more details.
 *
 * @param referencePattern [in] Pattern to refine against
 * @param toRefine [in] What parameters should be refined (besides background)
 */
void CalculatedPattern::rietveldRefinement(const Diffraction& referencePattern, std::set<RefinementParameters> toRefine) {
	if (!structureIsDefined()) {
        Output::newline(ERROR);
        Output::print("Internal Error: Structure not yet defined.");
    }
    Output::increase();
	
	// Clear what parameters are currently being refined
	_currentlyRefining.clear();
    
    // Create a list to store everything that can be refined at a certain point
    //  This will allow us to, for instance, refine a new parameter by itself
    //  and then refine with all other parameters considered so far
    std::set<RefinementParameters> refinedSoFar;
	
	// Get reference and calculated intensities (used for guesses)
	calculatePeakIntensities();
	vector<double> refAngles = referencePattern.getMeasurementAngles();
	vector<double> refIntensities = referencePattern.getMeasuredIntensities();
	vector<double> thisIntensities = getDiffractedIntensity(refAngles);
	
	// Refine the scale factor
	_currentlyRefining.insert(RF_SCALE);
    refinedSoFar.insert(RF_SCALE);
	double scaleGuess = *max_element(refIntensities.begin(), refIntensities.end()) /
			*max_element(thisIntensities.begin(), thisIntensities.end());
	_optimalScale = scaleGuess;
	double curR = runRefinement(&referencePattern, true);
	if (DIFFRACTION_EXCESSIVE_PRINTING) {
		thisIntensities = getDiffractedIntensity(refAngles);
		for (int i=0; i<thisIntensities.size(); i++) thisIntensities[i] *= _optimalScale;
		savePattern("rietveld-scale.pattern", refAngles, refIntensities, thisIntensities);
	}
	Output::newline();
	Output::print("Refined scale factor. Current R: ");
	Output::print(curR, 4);
	
	// Refine specimen displacement
    _currentlyRefining.clear();
	_currentlyRefining.insert(RF_SPECDISP);
    refinedSoFar.insert(RF_SPECDISP);
	curR = runRefinement(&referencePattern, true);
    _currentlyRefining.insert(refinedSoFar.begin(), refinedSoFar.end());
    curR = runRefinement(&referencePattern, true);
	Output::newline();
	Output::print("Refined specimen displacement. Current R: ");
	Output::print(curR, 4);
	
	// Refine the background
    _currentlyRefining.clear();
	_currentlyRefining.insert(RF_BACKGROUND);
    _currentlyRefining.insert(RF_SCALE);
    refinedSoFar.insert(RF_BACKGROUND);
	_backgroundParameters = guessBackgroundParameters(refAngles, refIntensities);
	if (DIFFRACTION_EXCESSIVE_PRINTING) {
		thisIntensities = getDiffractedIntensity(refAngles);
		for (int i=0; i<thisIntensities.size(); i++) thisIntensities[i] *= _optimalScale;
		savePattern("rietveld-background-guess.pattern", refAngles, refIntensities, thisIntensities);
	}
	curR = runRefinement(&referencePattern, true);
    _currentlyRefining.insert(refinedSoFar.begin(), refinedSoFar.end());
    curR = runRefinement(&referencePattern, true);
	if (DIFFRACTION_EXCESSIVE_PRINTING) {
		thisIntensities = getDiffractedIntensity(refAngles);
		for (int i=0; i<thisIntensities.size(); i++) thisIntensities[i] *= _optimalScale;
		savePattern("rietveld-background-fitted.pattern", refAngles, refIntensities, thisIntensities);
	}
	Output::newline();
	Output::print("Refined background functions. Current R: ");
	Output::print(curR, 4);
    
    // Refine lattice parameters, if desired
    if (_maxLatChange > 0) {
        std::set<RefinementParameters> oldParams;
        oldParams.insert(_currentlyRefining.begin(), _currentlyRefining.end());
        _currentlyRefining.clear();
        _currentlyRefining.insert(RF_BASIS);
        refinedSoFar.insert(RF_BASIS);
        curR = runRefinement(&referencePattern, true);
        _currentlyRefining.insert(oldParams.begin(), oldParams.end());
        Output::newline();
        Output::print("Refined lattice parameters. Current R: ");
        Output::print(curR, 4);
    }
	
	// Refine the peak broadening (W)
    W = guessPeakWidthParameter(refAngles, refIntensities);
	_currentlyRefining.insert(RF_WFACTOR);
    refinedSoFar.insert(RF_WFACTOR);
	curR = runRefinement(&referencePattern, true);
    _currentlyRefining.insert(refinedSoFar.begin(), refinedSoFar.end());
    curR = runRefinement(&referencePattern, true);
	Output::newline();
	Output::print("Refined peak-broadening term to ");
	Output::print(W, 4);
	Output::print(" degrees. Current R: ");
	Output::print(curR, 4);
    if (DIFFRACTION_EXCESSIVE_PRINTING) {
		thisIntensities = getDiffractedIntensity(refAngles);
		for (int i=0; i<thisIntensities.size(); i++) thisIntensities[i] *= _optimalScale;
		savePattern("rietveld-width-fitted.pattern", refAngles, refIntensities, thisIntensities);
	}
	if (curR > 0.9) {
		Output::newline();
		Output::print("Very poor pattern match, not refining further.");
		Output::decrease();
		return;
	}
	
	// Refine atomic positions, if desired
	if (willRefine(RF_POSITIONS, toRefine)) {
		_currentlyRefining.insert(RF_POSITIONS);
		curR = runRefinement(&referencePattern, true);
		Output::newline();
		Output::print("Refined atomic positions. Current R: ");
		Output::print(curR, 4);
	}
	
	// Refine preferred orientation factor
	_currentlyRefining.insert(RF_TEXTURE);
	curR = runRefinement(&referencePattern, true);
	Output::newline();
	Output::print("Refined preferred orientation factor. Magnitude is ");
	Output::print(_preferredOrientation.magnitude(), 3);
	Output::print(". Current R: ");
	Output::print(curR, 4);
	
	// Refine B factors, if desired
	if (willRefine(RF_BFACTORS, toRefine)) {
		_currentlyRefining.insert(RF_BFACTORS);
		curR = runRefinement(&referencePattern, true);
		Output::newline();
		Output::print("Refined B factors. Current R: ");
		Output::print(curR, 4);
	}
	
	// Refine everything else:
	_currentlyRefining.insert(RF_UVFACTORS);
	curR = runRefinement(&referencePattern, true);
	Output::newline();
	Output::print("Refined all broadening factors. Current R: ");
	Output::print(curR, 4);
	
	_currentlyRefining.insert(RF_ZEROSHIFT);
	curR = runRefinement(&referencePattern, true);
	Output::newline();
	Output::print("Refined zero shift. Current R: ");
	Output::print(curR, 4);
	
	if (DIFFRACTION_EXCESSIVE_PRINTING) {
		thisIntensities = getDiffractedIntensity(refAngles);
		for (int i=0; i<thisIntensities.size(); i++) thisIntensities[i] *= _optimalScale;
		savePattern("rietveld-final.pattern", refAngles, refIntensities, thisIntensities);
	}
	
	Output::decrease();
}

/**
 * Refine a structure (which has already been stored internally) against a reference
 *  diffraction pattern
 * 
 * LW 13Aug14: ToDo: Either pass Diffraction as ptr or by reference, not both!
 * 
 * @param reference [in] Pattern to refine against
 * @param toRefine [in] What parameters of the diffracted intensity should be refined
 */
void CalculatedPattern::refineParameters(const Diffraction* reference, std::set<RefinementParameters> toRefine) {
    if (!structureIsDefined()) {
        Output::newline(ERROR);
        Output::print("Internal Error: Structure not yet defined.");
    }
    Output::increase();

    // Clear what parameters are currently being refined
    _currentlyRefining.clear();

    // Optimize atomic positions, if desired
    if (willRefine(RF_POSITIONS, toRefine)) {
        Output::newline();
        Output::print("Refining atomic positions. Current R Factor: ");
        _currentlyRefining.insert(RF_POSITIONS);
        double curRFactor = runRefinement(reference, false);
        Output::print(curRFactor, 3);
    }

    // Next, refine both B factors and atomic positions
    if (willRefine(RF_BFACTORS, toRefine)) {
        Output::newline();
        Output::print("Also refining isotropic thermal factors. Current R Factor: ");
        _currentlyRefining.insert(RF_BFACTORS);
        double curRFactor = runRefinement(reference, false);
        Output::print(curRFactor, 3);
    }
    Output::decrease();

    // Output results
    if (willRefine(RF_BFACTORS, toRefine)) {
        for (int i = 0; i < _BFactors.size(); ++i) {
            Output::newline();
            Output::print("Optimized B factor for atom ");
            Output::print(_symmetry->orbits()[i].atoms()[0]->atomNumber() + 1);
            Output::print(" (");
            Output::print(_symmetry->orbits()[i].atoms()[0]->element().symbol());
            Output::print("): ");
            Output::print(_BFactors[i]);
        }
    }
    if (willRefine(RF_POSITIONS, toRefine)) {
        for (int i = 0; i < _symmetry->orbits().length(); ++i) {
            Output::newline();
            Output::print("Optimized position for atom ");
            Output::print(_symmetry->orbits()[i].atoms()[0]->atomNumber() + 1);
            Output::print(" (");
            Output::print(_symmetry->orbits()[i].atoms()[0]->element().symbol());
            Output::print("): ");
            for (int j = 0; j < 3; ++j) {
                Output::print(_symmetry->orbits()[i].atoms()[0]->fractional()[j]);
                if (j != 2)
                    Output::print(", ");
            }
        }
    }
}

/**
 * Called from refineParameters. Refine any parameters currently defined in 
 *  _currentlyRefining.
 * @param reference [in] Pattern to refine against
 * @param rietveld [in] Whether to do full-pattern refinement
 * @return Minimal R factor (using DR_ABS)
 */
double CalculatedPattern::runRefinement(const Diffraction* reference, bool rietveld) {
    column_vector params;
    column_vector x_low = getRefinementParameterLowerBoundary();
    column_vector x_high = getRefinementParameterUpperBoundary();
    params = getRefinementParameters();
    RFactorFunctionModel f(this, reference, rietveld);
    // Technical issue (as of 12Mar15): 
    //  Numerical derivatives are currently being used. Performance could be better
    //   with analytical derivatives. One would create a functional very similar
    //   to the version currently being used to pass the R function value in order
    //   to make analytical derivatives work with this code. 
    //      May the calculus be with you.
    // RFactorDerivativeFunctionalModel der(this); // Analytical derivatives were once used
    dlib::find_min_box_constrained(dlib::bfgs_search_strategy(),
            dlib::objective_delta_stop_strategy(1e-12, params.nr() * 30),
            f, dlib::derivative(f, 1e-6), params, x_low, x_high);
    setAccordingToParameters(params);
    calculatePeakIntensities();
	if (rietveld) {
		return getRietveldRFactor(*reference, DR_ABS);
	} else {
		return getCurrentRFactor(*reference, DR_ABS);
	}
}

/**
 * Get a vector representing the parameters which are being refined. Always arranged
 *  in the following order:
 * <ol>
 * <li>Scale factor</li>
 * <li>Specimen displacement parameter</li>
 * <li>Background parameters</li>
 * <li>Lattice parameters (lengths, then angles)</li> 
 * <li>Angle-dependent peak broadening/shape terms (U,V,eta1,eta2) </li>
 * <li>Angle-independent peak broadening/shape term (W, eta0)</li>
 * <li>Atomic positions</li>
 * <li>Thermal factors</li>
 * <li>Texturing parameters</li>
 * <li>Zero shift parameter</li>
 * </ol>
 * @return Current values of parameters to be optimized
 */
CalculatedPattern::column_vector CalculatedPattern::getRefinementParameters() {
    queue<double> params;
	if (willRefine(RF_SCALE, _currentlyRefining)) {
		params.push(_optimalScale);
	}
	if (willRefine(RF_SPECDISP, _currentlyRefining)) {
		params.push(_shiftParameters[4]);
	}
	if (willRefine(RF_BACKGROUND, _currentlyRefining)) {
		for (int p=0; p<_backgroundParameters.size(); p++) {
			params.push(_backgroundParameters[p]);
		}
	}
    if (willRefine(RF_BASIS, _currentlyRefining)) {
        Vector3D data = _structure->basis().lengths();
        for (int i=0; i<3; i++) {
            params.push(data[i]);
        }
        data = _structure->basis().angles();
        for (int i=0; i<3; i++) {
            params.push(data[i]);
        }
    }
	if (willRefine(RF_UVFACTORS, _currentlyRefining)) {
		params.push(_U); params.push(V);
		params.push(_eta1); params.push(_eta2);
	}
	if (willRefine(RF_WFACTOR, _currentlyRefining)) {
		params.push(W); params.push(_eta0);
	}
    if (willRefine(RF_POSITIONS, _currentlyRefining)) {
        for (int orbit = 0; orbit < _symmetry->orbits().length(); orbit++)
            for (int dir = 0; dir < 3; dir++) {
                double pos = _symmetry->orbits()[orbit].atoms()[0]->fractional()[dir];
                params.push(pos);
            }
    }
    if (willRefine(RF_BFACTORS, _currentlyRefining)) {
        for (int i = 0; i < _BFactors.size(); i++)
            params.push(_BFactors[i]);
    }
	if (willRefine(RF_TEXTURE, _currentlyRefining)) {
		for (int i = 0; i < 3; i++) {
			params.push(_preferredOrientation[i]);
		}
	}
	if (willRefine(RF_ZEROSHIFT, _currentlyRefining)) {
		params.push(_shiftParameters[5]);
	}

    // Copy parameters to an appropriate container
    int nParams = params.size();
    column_vector output(nParams);
    for (int i = 0; i < nParams; i++) {
        output(i) = params.front();
        params.pop();
    }
    return output;
}

/**
 * Get the lower boundary of each refinement parameter. 
 * @return Column vector detailing lower boundary for each parameter, same order
 *  as getRefinementParameters.
 */
CalculatedPattern::column_vector CalculatedPattern::getRefinementParameterLowerBoundary() {
    queue<double> params;
	if (willRefine(RF_SCALE, _currentlyRefining)) {
		params.push(0);
	}
	if (willRefine(RF_SPECDISP, _currentlyRefining)) {
		params.push(-0.1);
	}
	if (willRefine(RF_BACKGROUND, _currentlyRefining)) {
		for (int i=0; i<_backgroundParameters.size(); i++) {
			params.push(-1e100);
		}
	}
    if (willRefine(RF_BASIS, _currentlyRefining)) {
        Vector3D data = _originalLengths;
        for (int i=0; i<3; i++) {
            params.push(data[i] * (1 - _maxLatChange));
        }
        data = _originalAngles;
        for (int i=0; i<3; i++) {
            params.push(data[i] * (1 - _maxLatChange));
        }
    }
	if (willRefine(RF_UVFACTORS, _currentlyRefining)) {
		params.push(-1e100); params.push(-1e100);
		params.push(-1e100); params.push(-1e100);
	}
	if (willRefine(RF_WFACTOR, _currentlyRefining)) {
		params.push(0); params.push(0);
	}
    if (willRefine(RF_POSITIONS, _currentlyRefining))
        for (int i = 0; i < _symmetry->orbits().length() * 3; i++)
            params.push(-1);
    if (willRefine(RF_BFACTORS, _currentlyRefining))
        for (int i = 0; i < _BFactors.size(); i++)
            params.push(_minBFactor);
	if (willRefine(RF_TEXTURE, _currentlyRefining)) {
		for (int i = 0; i < 3; i++) {
			params.push(-10);
		}
	}
	if (willRefine(RF_ZEROSHIFT, _currentlyRefining)) {
		params.push(-0.1);
	}
    // Copy parameters to an appropriate container
    int nParams = params.size();
    column_vector output(nParams);
    for (int i = 0; i < nParams; i++) {
        output(i) = params.front();
        params.pop();
    }
    return output;
}

/**
 * Get the upper boundary of each refinement parameter. 
 * @return Column vector detailing upper boundary for each parameter, same order
 *  as getRefinementParameters.
 */
CalculatedPattern::column_vector CalculatedPattern::getRefinementParameterUpperBoundary() {
    queue<double> params;
	if (willRefine(RF_SCALE, _currentlyRefining)) {
		params.push(1e100);
	}
	if (willRefine(RF_SPECDISP, _currentlyRefining)) {
		params.push(0.1);
	}
	if (willRefine(RF_BACKGROUND, _currentlyRefining)) {
		for (int i=0; i<_backgroundParameters.size(); i++) {
			params.push(1e100);
		}
	}
    if (willRefine(RF_BASIS, _currentlyRefining)) {
        Vector3D data = _originalLengths;
        for (int i=0; i<3; i++) {
            params.push(data[i] * (1 + _maxLatChange));
        }
        data = _originalAngles;
        for (int i=0; i<3; i++) {
            params.push(data[i] * (1 + _maxLatChange));
        }
    }
	if (willRefine(RF_UVFACTORS, _currentlyRefining)) {
		params.push(1e100); params.push(1e100);
		params.push(1e100); params.push(1e100);
	}
	if (willRefine(RF_WFACTOR, _currentlyRefining)) {
		params.push(20); params.push(1);
	}
    if (willRefine(RF_POSITIONS, _currentlyRefining))
        for (int i = 0; i < _symmetry->orbits().length() * 3; i++)
            params.push(2);
    if (willRefine(RF_BFACTORS, _currentlyRefining))
        for (int i = 0; i < _BFactors.size(); i++)
            params.push(_maxBFactor);
	if (willRefine(RF_TEXTURE, _currentlyRefining)) {
		for (int i = 0; i < 3; i++) {
			params.push(10);
		}
	}
	if (willRefine(RF_ZEROSHIFT, _currentlyRefining)) {
		params.push(0.1);
	}
    // Copy parameters to an appropriate container
    int nParams = params.size();
    column_vector output(nParams);
    for (int i = 0; i < nParams; i++) {
        output(i) = params.front();
        params.pop();
    }
    return output;
}

/**
 * Set all refinement parameters according to those contained in an input vector.
 *  The values in this vector depending on which parameters are _currentlyRefining. 
 *  Their order should be the same as defined in getCurrentParameters
 *  
 * @param params [in] New values of parameters being refined
 */
void CalculatedPattern::setAccordingToParameters(column_vector params) {
    int position = 0;
	if (willRefine(RF_SCALE, _currentlyRefining)) {
		_optimalScale = params(position++);
	}
	if (willRefine(RF_SPECDISP, _currentlyRefining)) {
		_shiftParameters[4] = params(position++);
	}
	if (willRefine(RF_BACKGROUND, _currentlyRefining)) {
		for (int i=0; i<_backgroundParameters.size(); i++) {
			_backgroundParameters[i] = params(position++);
		}
	}
    if (willRefine(RF_BASIS, _currentlyRefining)) {
        vector<double> newParams;
        for (int i=0; i < 6; i++) {
            newParams.push_back(params(position++));
        }
        setBasis(newParams);
    }
	if (willRefine(RF_UVFACTORS, _currentlyRefining)) {
		U = params(position++);
		V = params(position++);
		_eta1 = params(position++);
		_eta2 = params(position++);
	}
	if (willRefine(RF_WFACTOR, _currentlyRefining)) {
		W = params(position++); _eta0 = params(position++);
	}
    if (willRefine(RF_POSITIONS, _currentlyRefining)) {
        Vector newPositions(_symmetry->orbits().length()*3);
        for (int i = 0; i < newPositions.length(); i++) {
            newPositions[i] = params(position++);
        }
        symPositions(*_symmetry, newPositions);
        setPositions(*_symmetry, newPositions);
    }
    if (willRefine(RF_BFACTORS, _currentlyRefining)) {
        for (int i = 0; i < _BFactors.size(); i++)
            _BFactors[i] = params(position++);
    }
	if (willRefine(RF_TEXTURE, _currentlyRefining)) {
		for (int i = 0; i < 3; i++) {
			_preferredOrientation[i] = params(position++);
		}
	}
	if (willRefine(RF_ZEROSHIFT, _currentlyRefining)) {
		_shiftParameters[5] = params(position++);
	}
}

/**
 * Refine a structure against reference pattern
 * 
 * @param iso [in,out] Structure to be refined. Returns refined coordinates
 * @param symmetry [in,out] Symmetry information about structure. Returns refined coordinates
 * @param reference [in] Pattern to refine against
 * @param rietveld [in] Whether to perform full-pattern refinement
 * @param showWarnings [in] Whether to print warnings
 * @return Optimized R Factor
 */
double CalculatedPattern::refine(ISO& iso, Symmetry& symmetry, const Diffraction& reference, bool rietveld,  bool showWarnings) {
    // Clear out any old information
    clear();
    
    // Store the pattern information  
    defineReferencePattern(reference);
	
    // Store structure and initialize guesses
    defineStructure(iso, symmetry);

    // Output
    Output::newline();
    Output::print("Refining structure against reference pattern");
    Output::increase();

    // Refine B factors, intensity scale, and positions
    std::set<RefinementParameters> toRefine;
    toRefine.insert(RF_BFACTORS);
    toRefine.insert(RF_POSITIONS);

    // Run refinement
	double rFactor;
	if (rietveld) {
		rietveldRefinement(reference, toRefine);
		rFactor = getRietveldRFactor(reference, DR_ABS);
	} else {
		matchPeaksToReference(reference);
		refineParameters(&reference, toRefine);
		rFactor = getCurrentRFactor(reference, DR_ABS);
	}

    // Output
    Output::newline();
    Output::print("Optimal R factor: ");
    Output::print(rFactor);

    // Output
    Output::decrease();

    // Return R factor
    return rFactor;
}

/** 
 * Calculate peaks that will appear in diffraction pattern of a 
 * particular structure. Store them internally in _integratedPeaks.
 */
void CalculatedPattern::calculatePeakLocations() {
	Output::increase();
	// Clear any previously-calculated peaks
	_reflections.clear();

	// Calculate the hkl range
	int i, j;
	double range[3];
	double maxMag = 2 * sin(Num<double>::toRadians(_maxTwoTheta / 2)) / _wavelength;
	Vector3D vec;
	for (i = 0; i < 3; ++i) {
		for (j = 0; j < 3; ++j)
			vec[j] = _structure->basis().reducedInverse()(j, i);
		range[i] = Num<double>::abs(Num<double>::ceil(maxMag / vec.magnitude()));
	}

	// Conversion matrix to take reduced cell reciprocal lattice vector to unit cell reciprocal lattice vector
	Matrix3D convHKL = _structure->basis().unitPointToReduced().transpose();

	// Generate symmetry operations for reduced cell
	Matrix3D P = _structure->basis().unitToReduced().transpose();
	Matrix3D Q = P.inverse();
	OList<Matrix3D > operations(_symmetry->operations().length());
	for (i = 0; i < _symmetry->operations().length(); ++i) {
		operations[i] = P;
		operations[i] *= _symmetry->operations()[i].rotation();
		operations[i] *= Q;
		operations[i] = operations[i].transpose();
	}

	// Remove identity operation
	Matrix3D identity;
	identity.makeIdentity();
	for (i = 0; i < operations.length(); ++i) {
		if (operations[i] == identity) {
			operations.remove(i);
			break;
		}
	}

	// Get the intrinsic part of all symmetry operations
	OList<Vector3D >::D2 translations(_symmetry->operations().length());
	for (i = 0; i < _symmetry->operations().length(); ++i) {
		translations[i].length(_symmetry->operations()[i].translations().length());
		for (j = 0; j < _symmetry->operations()[i].translations().length(); ++j)
			translations[i][j] = Symmetry::intrinsicTranslation(_symmetry->operations()[i].rotation(), \
				_symmetry->operations()[i].translations()[j]);
	}

	// Get the peak intensities
	bool found;
	int mult;
	double product;
	double twoTheta;
	Vector3D hkl;
	Vector3D redHKL;
	Vector3D symHKL;
	Linked<Vector3D > equivPoints;
	Linked<Vector3D >::iterator itEquiv;
	for (redHKL[0] = -range[0]; redHKL[0] <= range[0]; ++redHKL[0]) {
		for (redHKL[1] = -range[1]; redHKL[1] <= range[1]; ++redHKL[1]) {
			for (redHKL[2] = -range[2]; redHKL[2] <= range[2]; ++redHKL[2]) {
				// Loop over operations to generate equivalent points
				mult = 1;
				equivPoints.clear();
				equivPoints += redHKL;
				for (i = 0; i < operations.length(); ++i) {
					// Get new point
					symHKL = operations[i] * redHKL;
					for (j = 0; j < 3; ++j)
						symHKL[j] = Num<double>::round(symHKL[j], 1);

					// Check if hkl is equilvalent of something that has been generated earlier
					// If so, set mult = 0
					if (symHKL[0] < redHKL[0] - 1e-4)
						mult = 0;
					else if (Num<double>::abs(symHKL[0] - redHKL[0]) < 1e-4) {
						if (symHKL[1] < redHKL[1] - 1e-4)
							mult = 0;
						else if (Num<double>::abs(symHKL[1] - redHKL[1]) < 1e-4) {
							if (symHKL[2] < redHKL[2] - 1e-4)
								mult = 0;
						}
					}
					if (mult == 0)
						break;

					// Check if point is already known as an equivalent point
					found = false;
					for (itEquiv = equivPoints.begin(); itEquiv != equivPoints.end(); ++itEquiv) {
						if ((Num<double>::abs((*itEquiv)[0] - symHKL[0]) < 1e-4) && \
                                (Num<double>::abs((*itEquiv)[1] - symHKL[1]) < 1e-4) && \
                                (Num<double>::abs((*itEquiv)[2] - symHKL[2]) < 1e-4)) {
							found = true;
							break;
						}
					}
					if (!found) {
						++mult;
						equivPoints += symHKL;
					}
				}

				// Multiplier is zero so skip
				if (mult == 0)
					continue;

				// Convert current reduced basis hkl to unit cell
				hkl = convHKL * redHKL;
				vector<Vector3D> equivHKL;
				equivHKL.reserve(mult);
				for (itEquiv = equivPoints.begin(); itEquiv != equivPoints.end(); ++itEquiv) {
					*itEquiv = convHKL * *itEquiv;
					equivHKL.push_back(*itEquiv);
				}

				// Check if direction will be a systematic absence
				found = false;
				for (i = 0; i < _symmetry->operations().length(); ++i) {

					// Check if R*hkl = hkl
					symHKL = _symmetry->operations()[i].rotation() * hkl;
					if ((Num<double>::abs(symHKL[0] - hkl[0]) > 1e-4) || \
                            (Num<double>::abs(symHKL[1] - hkl[1]) > 1e-4) || \
                            (Num<double>::abs(symHKL[2] - hkl[2]) > 1e-4))
						continue;

					// Loop over intrinsic translations and check if ti*hkl = integer for all
					for (j = 0; j < translations[i].length(); ++j) {
						product = translations[i][j] * hkl;
						if (Num<double>::abs(Num<double>::round(product, 1) - product) > 1e-4) {
							found = true;
							break;
						}
					}

					// Break if hkl is a system absence
					if (found)
						break;
				}

				// Current hkl will be a systematic absence (keep just in case)
				// if (found)
				//continue;

				// Get the current angle
				twoTheta = 2 * Num<double>::toDegrees(CalculatedPeak::getDiffractionAngle(_structure->basis(), hkl, wavelength()));

				// Skip if diffraction angle is too small or too large
				if ((twoTheta < _minTwoTheta) || (twoTheta > _maxTwoTheta))
					continue;

				// Add peak to list of known peaks
				CalculatedPeak newPeak(method(), this->_structure, this->_symmetry, this->wavelength(), hkl, equivHKL);
				this->_reflections.push_back(newPeak);
			}
		}
	}

	// Ensure peaks are in proper order
	std::sort(_reflections.begin(), _reflections.end());

	Output::newline();
	Output::print("Total number of peaks: ");
	Output::print(_reflections.size());
	Output::decrease();
}

/**
 * Calculate the peak intensities, given complete information about a structure. Stores
 * resulting peak heights internally. 
 */
void CalculatedPattern::calculatePeakIntensities() {
	double texturingParameter = _preferredOrientation.magnitude();
    for (int i = 0; i < _reflections.size(); i++) {
        // Update peak positions if they are currently being refined
        if (willRefine(RF_BASIS, _currentlyRefining)) {
            _reflections[i].updatePeakPosition();
        }
        
        // Update peak intensities
        _reflections[i].updateCalculatedIntensity(_BFactors, _atfParams, _preferredOrientation, texturingParameter);
    }
}

/**
 * Update the calculated integrated intensity for a diffraction peak. Here, the user must provide
 *  all non-structural, refinable variables. Structural variables are provided via links to the 
 *  structure associated with each peak. 
 * @param BFactors [in] B Factors for each site
 * @param atfParams [in] Atomic form factors for each element (stored as a property of the pattern to conserve memory)
 */
void CalculatedPeak::updateCalculatedIntensity(vector<double>& BFactors, List<double>::D2& atfParams,
		Vector3D& preferredOrientation, double texturingStrength) {
    // Calculate integrated intensity. (Note absence of scale factor, which 
    //  is always optimized when calculating R factor)
    peakIntensity = CalculatedPeak::structureFactorSquared(method, wavelength, *symmetry, 
			_twoThetaRad / 2, hkl, BFactors, atfParams);
    // Everything but the structure factor
    peakIntensity *= lpFactor;
	peakIntensity *= multiplicity;
	peakIntensity *= getTexturingFactor(preferredOrientation, texturingStrength, recipLatVecs);
}

vector<DiffractionPeak> ExperimentalPattern::getDiffractedPeaks() const {
	if (_diffractionPeaks.size() == 0) {
		Output::newline(ERROR);
		Output::print("No diffracted intensities were set. Something might have failed during import.");
		Output::quit();
	}
	vector<DiffractionPeak> output;
	output.insert(output.begin(), _diffractionPeaks.begin(), _diffractionPeaks.end());
	return output;
}

vector<double> ExperimentalPattern::getDiffractedIntensity(vector<double>& twoTheta) const {
	// Sort input array
	std::sort(twoTheta.begin(), twoTheta.end());
	
	// Check bounds
	if (twoTheta[0] < _continuousTwoTheta[0]) {
		Output::newline(ERROR);
		Output::print("No data before ");
		Output::print(_continuousTwoTheta[0]);
		Output::quit();
	}
	if (twoTheta.back() > _continuousTwoTheta[_continuousTwoTheta.size() - 1]) {
		Output::newline(ERROR);
		Output::print("No data after ");
		Output::print(_continuousTwoTheta[_continuousTwoTheta.size() - 1]);
		Output::quit();
	}
	
	// Compute intensity at each point using linear interpolation
	vector<double> output;
	output.reserve(twoTheta.size());
	int cpos = 0;
	for (int a=0; a<twoTheta.size(); a++) {
		double angle = twoTheta[a];
		while (_continuousTwoTheta[cpos + 1] < angle) {
			cpos++;
		}
		double intensity = _continuousIntensity[cpos] + (_continuousIntensity[cpos + 1] - _continuousIntensity[cpos])
			/ (_continuousTwoTheta[cpos + 1] - _continuousTwoTheta[cpos]) * (angle - _continuousTwoTheta[cpos]);
		output.push_back(intensity);
	}
	return output;
}

vector<double> CalculatedPattern::getDiffractedIntensity(vector<double>& twoTheta) const {
	vector<double> output = generateBackgroundSignal(twoTheta);
	vector<double> signal = generatePeakSignal(twoTheta);
	for (int i=0; i<output.size(); i++) {
		output[i] += signal[i];
	}
	return output;
}

/**
 * Generate the diffraction signal associated with diffraction peaks
 * @param twoTheta [in] Angle at which peak intensity will be calculated
 * @return Peak intensity at those angles
 */
vector<double> CalculatedPattern::generatePeakSignal(vector<double>& twoTheta) const {
	vector<double> output; output.insert(output.begin(), twoTheta.size(), 0);
	// Useful constants 
	double Cg = 4 * log(2);
	double rCg = sqrt(Cg);
	double rPI = sqrt(M_PI);
	// Add in signal from each peak
	int startAngle = 0;
	for (int p=0; p<_reflections.size(); p++) {
		double center = _reflections[p].getAngle();
		double centerRad = _reflections[p].getAngleRadians();
		// Compute peak-broadening terms
		double H = W + tan(centerRad / 2) 
				* (V + U * tan(centerRad / 2));
		H = sqrt(H);
		
		// Compute mixing parameters
		double eta = _eta0 + center * (_eta1 + center * _eta2);
		
		// Compute peak shift (from calculated to observed position)
		double shift = _shiftParameters[0] / tan(centerRad) + _shiftParameters[1] / sin(centerRad)
				+ _shiftParameters[2] / tan(centerRad / 2) + _shiftParameters[3] * sin(centerRad)
				+ _shiftParameters[4] * cos(centerRad) + _shiftParameters[5];
		center += shift;
		
		// Determine the range on which we will bother computing the integral;
		double minAngle = center - 6.0 * H;
		double maxAngle = center + 6.0 * H;
		if (minAngle >= maxTwoTheta()) continue;
		
		// Compute broadened pattern
		int a = lower_bound(twoTheta.begin(), twoTheta.end(), minAngle) - twoTheta.begin();
		double intensity = _reflections[p].getIntensity();
		while (twoTheta[++a] < minAngle) continue;
		startAngle = a;
		double x, gaussian, lorentzian;
		double gPrefactor = rCg / rPI / H, lPrefactor = 2.0 / M_PI / H;
		while (twoTheta[a] < maxAngle && a < twoTheta.size()) {
			x = pow((twoTheta[a] - center) / H, 2.0);
			gaussian = gPrefactor * exp(-Cg * x);
			lorentzian = lPrefactor / (1 + 4.0 * x);
			output[a] += intensity * (eta * gaussian + (1 - eta) * lorentzian);
			a++;
		}
	}
	return output;
}


/**
 * Calculate the background signal as a function of angle. Functional form:
 * 
 * I(x) = c_0 / x + c_1 + c_2 * x + c_3 * x ^ 2 + ... + c_n * x ^ (n - 1)
 * 
 * Background parameters are stored in: _backgroundParameters
 * 
 * @param twoTheta Angle at which to calculate background.
 * @return Background intensity at each point
 */
vector<double> CalculatedPattern::generateBackgroundSignal(vector<double>& twoTheta) const {
	// Initialize blank output
	vector<double> output;
	output.insert(output.begin(), twoTheta.size(), 0.0);
	if (_backgroundParameters.empty()) return output; // No background
	
	// Create array used when computing Chebyshev polynomial values
	double chebyshev[_numBackground];
	chebyshev[0] = 1;
	
	// Add in polynomial terms
	for (int a=0; a<twoTheta.size(); a++) {		
		if (_useChebyshev) {
			output[a] += _backgroundParameters[0];
			if (_backgroundParameters.size() == 1) continue;
			double x = 2 * (twoTheta[a] - _minTwoTheta) / (_maxTwoTheta - _minTwoTheta) - 1;
			chebyshev[1] = x;
			output[a] += _backgroundParameters[1] * chebyshev[1];
			for (int t=2; t<_backgroundParameters.size(); t++) {
				chebyshev[t] = 2 * x * chebyshev[t-1] - chebyshev[t-2];
				output[a] += _backgroundParameters[t] * chebyshev[t];
			}
		} else {
			double x = pow(twoTheta[a], _backgroundPolyStart);
			for (int p=0; p<_backgroundParameters.size(); p++) {
				output[a] += _backgroundParameters[p] * x;
				x *= twoTheta[a];
			}
		} 
	}
	return output;
}

/**
 * Generates guess for background parameters. 
 * @param twoTheta Angles at which to calculate error signal
 * @param refIntensities Reference intensity value at those angles
 * @return Guess for background signal
 */
vector<double> CalculatedPattern::guessBackgroundParameters(vector<double>& twoTheta,
		vector<double>& refIntensities) {	
	// Mark which entries to use in fitting
	vector<double> fitAngles, fitIntensities;
	fitAngles.reserve(twoTheta.size());
	fitIntensities.reserve(twoTheta.size());
	int pos = 0;
    double patternWidth = _reflections.back().getAngle() - 
            _reflections.front().getAngle();
	for (int peak=0; peak < _reflections.size(); peak++) {
		while (twoTheta[pos] < _reflections[peak].getAngle() - patternWidth / 100) {
			fitAngles.push_back(twoTheta[pos]);
			fitIntensities.push_back(refIntensities[pos]);
			pos++;
		}
		while (twoTheta[pos] < _reflections[peak].getAngle() + patternWidth / 100) {
			pos++;
		}
	}
	
	// If there are too many peaks, don't make any guesses
	if (fitAngles.size() < _numBackground * 100) {
		vector<double> output;
		output.resize(_numBackground, 0.0);
		return output;
	}
	
	// Guess the terms using polynomial fitting
	dlib::matrix<double> Y(fitIntensities.size(), 1), A(fitIntensities.size(), _numBackground);
	for (int i=0; i<fitIntensities.size(); i++) {
		Y(i, 0) = fitIntensities[i];
		if (_useChebyshev) {
			A(i,0) = 1;
			if (_numBackground < 2) continue;
			double x = 2 * (fitAngles[i] - _minTwoTheta) / (_maxTwoTheta - _minTwoTheta) - 1;
			A(i,1) = x;
			for (int j=2; j<_numBackground; j++) {
				A(i,j) = 2 * x * A(i,j-1) - A(i,j-2);
			}
		} else {
			double x = pow(fitAngles[i], (double) _backgroundPolyStart);
			for (int j=0; j<_numBackground; j++) {
				A(i,j) = x;
				x *= fitAngles[i];
			}
		} 
	}
	dlib::qr_decomposition<dlib::matrix<double> > solver(A);
	dlib::matrix<double> params = solver.solve(Y);
	
	// Return new values
	vector<double> output;
	output.reserve(_numBackground);
	for (int i=0; i<params.nr(); i++) {
		output.push_back(params(i,0) / _optimalScale);
	}
	return output;
}

/**
 * Guess a starting value for the peak width parameter.
 * @param twoTheta [in] Angles at which pattern was measured
 * @param referenceIntensities [out] Measured intensities
 * @return Guess for peak half width
 */
double CalculatedPattern::guessPeakWidthParameter(vector<double>& twoTheta, vector<double>& referenceIntensities) {
    // Get the maximum value of the pattern
    double halfMax = *std::max_element(referenceIntensities.begin(), 
            referenceIntensities.end()) / 2.0;
    
    // Go until the pattern is below half the maximum value
    int pos = 0;
    while (referenceIntensities[pos] > halfMax) {
        pos++;
    }
    
    // Now, count each time it crosses halfway
    vector<double> widths;
    bool isAbove = false;
    double startAngle = 0;
    while (pos < twoTheta.size()) {
        if (isAbove) {
            if (referenceIntensities[pos] < halfMax) {
                isAbove = false;
                widths.push_back(twoTheta.at(pos) - startAngle);
            }
        } else {
            if (referenceIntensities[pos] > halfMax) {
                isAbove = true;
                startAngle = twoTheta[pos];
            }
        }
        pos++;
    }
    
    // Compute mean width
    double mean = std::accumulate(widths.begin(), widths.end(), 0.0);
    mean /= widths.size();
    
    return mean > 1.0 ? 1.0 : mean;
}


/** 
 * Get combined / scaled peaks for prettier output.
 * @return List of peaks corresponding to summed groups of reflections
 */
vector<DiffractionPeak> CalculatedPattern::getCombinedPeaks() const {
    vector<double> tempTwoTheta; tempTwoTheta.reserve(_reflections.size());
    vector<double> tempIntensity; tempIntensity.reserve(_reflections.size());
    tempTwoTheta.push_back(_reflections[0].getAngle());
    tempIntensity.push_back(_reflections[0].getIntensity());
	double scaleFactor;
	
	// Build a list of intensities
    if (_matchingPeaks.size() > 0) { // If peaks have been matched
        // Combine peaks with the same pattern index (knowing that they are sorted)
        int lastPatternIndex = _reflections[0].patternIndex;
        for (int i=1; i<_reflections.size(); i++) {
            if (_reflections[i].patternIndex == -1 || 
                    _reflections[i].patternIndex != lastPatternIndex) {
                tempTwoTheta.push_back(_reflections[i].getAngle());
                tempIntensity.push_back(_reflections[i].getIntensity());
            } else 
                tempIntensity.back() += _reflections[i].getIntensity();
        }
		
		// Select scale factor maximum is 1000
		scaleFactor = 1000.0 / *std::max_element(tempIntensity.begin(), tempIntensity.end());
    } else {
        // Combine peaks closer than 0.15 degrees
        int lastAngle = -100;
        for (int i=1; i<_reflections.size(); i++) {
            if (_reflections[i].getAngle() - lastAngle > 0.15) {
                tempTwoTheta.push_back(_reflections[i].getAngle());
                tempIntensity.push_back(_reflections[i].getIntensity());
                lastAngle = tempTwoTheta.back();
            } else {
                tempIntensity.back() += _reflections[i].getIntensity();
            }
        }
		// Assume that these peaks have been scaled
		scaleFactor = 1.0;
    }
	
    // Save peaks
    vector<DiffractionPeak> output;
	output.reserve(tempTwoTheta.size());
	for (int i=0; i<tempTwoTheta.size(); i++) {
		output.push_back(DiffractionPeak(tempTwoTheta[i],tempIntensity[i] * scaleFactor));
	}
	
	return output;
}

void CalculatedPattern::matchPeaksToReference(const Diffraction& referencePattern) {
	// Before calling superclass, ensure that no peaks will be matched together
	for (int i=0; i<_reflections.size(); i++) {
		_reflections[i].patternIndex = i;
	}
	Diffraction::matchPeaksToReference(referencePattern);
}

/**
 * Given a diffraction pattern, identifies which calculated peaks match up to 
 *  each peak in the stored reference pattern. Groups of peaks that match a single peak
 *  in the reference and those peaks in this pattern that are not matched are stored 
 *  internally.
 * 
 */
void Diffraction::matchPeaksToReference(const Diffraction& _referencePattern) {
    
    // Reset lookup table for which peaks in this pattern match to a each 
    //  peak in the reference
	vector<DiffractionPeak> referencePeaks = _referencePattern.getDiffractedPeaks();
    _matchingPeaks.clear(); 
    _matchingPeaks.reserve(referencePeaks.size());
    for (int i=0; i<referencePeaks.size(); i++) {
        vector<int> empty;
        _matchingPeaks.push_back(empty);
    }
    
    // Clear list of peaks that don't match anything
    _unmatchedPeaks.clear();

    // Tolerance for peaks to be aligned
    double tol = 0.15;
	
	
	vector<DiffractionPeak> thisPeaks = getDiffractedPeaks();
    // Loop over diffraction peaks in this pattern
    for (int thisPeak = 0; thisPeak < thisPeaks.size(); ++thisPeak) {
        // Find the nearest diffraction peak in reference pattern
        int nearIndex = 0;
        double nearDif = abs(thisPeaks[thisPeak].getAngle() - \
                referencePeaks[thisPeak].getAngle());
        for (int refPeak = 1; refPeak < referencePeaks.size(); ++refPeak) {
            double curDif = abs(thisPeaks[thisPeak].getAngle() \
                - referencePeaks[refPeak].getAngle());
            if (curDif < nearDif) {
                nearIndex = refPeak;
                nearDif = curDif;
            }
        }

        // If no peak within tolerance of this peak mark its index as -1 (not matched)
        //  and move on to the next peak
        if (nearDif > tol) {
            thisPeaks[thisPeak].patternIndex = -1;
            _unmatchedPeaks.push_back(thisPeak);
            continue;
        }

        // Otherwise, save index of peak in reference as the pattern index for this peak
        thisPeaks[thisPeak].patternIndex = nearIndex;
        
        // Add this peak to the list of those that match nearIndex
        _matchingPeaks[nearIndex].push_back(thisPeak);
    }
}

/**
 * Calculate the squared structure factor for all atoms in a crystal for certain 
 *  plane at a specific angle.
 * @param method [in] Method used to calculate diffraction peaks
 * @param wavelength [in] Wavelength of incident radiation
 * @param symmetry [in] Symmetrical information about a structure
 * @param angle [in] Angle at which radiation is reflected (radians)
 * @param hkl [in] Crystallographic plane being considered
 * @param BFactors [in] Thermal factors for each orbit of atoms
 * @param atfParams [in] Parameters to atomic form factors
 * @return Squared structure factor for this condition
 */
double CalculatedPeak::structureFactorSquared(Method method, double wavelength, const Symmetry& symmetry, double angle, \
        const Vector3D& hkl, vector<double> BFactors, List<double>::D2 atfParams) {

    // Loop over all atoms and calculate magnitude squared
    int i, j, k;
    double dot;
    double pre;
    double real = 0;
    double imag = 0;
    double sinTerm;
    double cosTerm;
    double thermFactor;
    double scatteringFactor;
    Atom* curAtom;
	for (i = 0; i < symmetry.orbits().length(); ++i) {

		// Get scaling factors for current atom
		curAtom = symmetry.orbits()[i].atoms()[0];
		scatteringFactor = atomicScatteringFactor(atfParams[i], angle, wavelength);
		thermFactor = (method == DM_SIMPLE) ? 1 : thermalFactor(angle, BFactors[i]);
		
		// Loop over equivalent atoms
		for (j = 0; j < symmetry.orbits()[i].atoms().length(); ++j) {

			// Calculate contribution from current atom
			curAtom = symmetry.orbits()[i].atoms()[j];
			dot = 2 * Constants::pi * (hkl * curAtom->fractional());
			sinTerm = sin(dot);
			cosTerm = cos(dot);

			// Add contribution
			pre = scatteringFactor * thermFactor * curAtom->occupancy();
			real += pre * cosTerm;
			imag += pre * sinTerm;
		}
	}
	
    // Return the square of the magnitude
    return real * real + imag*imag;
}

/**
 * Given a list of angles and intensities, determine locations and intensities of 
 *  diffraction peaks. Store peak information internally.
 * 
 * If the intensities are uniformly spaced, this function assumes that it was provided
 *  with a raw diffraction pattern. It will subsequently remove the noise and background
 *  signal, and then detect and locate peaks. 
 * 
 * If the spacings are random (as far as the code is concerned), it will assume that
 *  the peak intensities have already been measured.
 * 
 * In either case, this object will contain the angles and integrated intensities of 
 *  each provided diffraction peak at the end of of the operation 
 * 
 * @param twoTheta [in] List of angles at which diffracted intensity is measured
 * @param intensity [in] Intensity measured at each angle
 */
void ExperimentalPattern::set(const Linked<double>& twoTheta, const Linked<double>& intensity) {
	// Copy over two theta
	vector<double> twoThetaCopy(twoTheta.length());
	Linked<double>::iterator iter = twoTheta.begin();
	for (int i = 0; i < twoTheta.length(); i++, iter++) twoThetaCopy[i] = *iter;

	// Copy over intensity
	vector<double> intensityCopy(intensity.length());
	iter = intensity.begin();
	for (int i = 0; i < intensity.length(); i++, iter++) intensityCopy[i] = *iter;

	// Call the real set function
	set(twoThetaCopy, intensityCopy);
}

/**
 * Given a list of angles and intensities, determine locations and intensities of 
 *  diffraction peaks. Store peak information internally.
 * 
 * If the intensities are uniformly spaced, this function assumes that it was provided
 *  with a raw diffraction pattern. It will subsequently remove the noise and background
 *  signal, and then detect and locate peaks. 
 * 
 * If the spacings are random (as far as the code is concerned), it will assume that
 *  the peak intensities have already been measured.
 * 
 * In either case, this object will contain the angles and integrated intensities of 
 *  each provided diffraction peak at the end of of the operation 
 * 
 * @param twoTheta [in, out] List of angles at which diffracted intensity is measured. Returned in sorted order
 * @param intensity [in] Intensity measured at each angle. Sorted in same order as twoTheta
 */
void ExperimentalPattern::set(vector<double>& twoTheta, vector<double>& intensity) {
    // Clear space
    clear();

    // Ensure that arrays are sorted in the ascending order.
    //  LW 8Apr14: Requires copying to pair vector and back, ick! It might be reasonable to 
    //             treat twoTheta and Intensity as a pair always. Consider for later
    vector<pair<double,double> > twoThetaIntensityPairs(twoTheta.size());
    for (int i=0; i<twoTheta.size(); i++) {
	pair<double,double> newPoint(twoTheta[i], intensity[i]);
	twoThetaIntensityPairs[i] = newPoint;
    }
    sort(twoThetaIntensityPairs.begin(), twoThetaIntensityPairs.end());
    for (int i=0; i<twoTheta.size(); i++) {
         twoTheta[i] = twoThetaIntensityPairs[i].first;
         intensity[i] = twoThetaIntensityPairs[i].second;
    }
    twoThetaIntensityPairs.clear();


	// Loop over two theta values and get max and min distances between them
	double curDif;
	double minDif = 0;
	double maxDif = 0;
	if (twoTheta.size() >= 2)
		minDif = maxDif = twoTheta[1] - twoTheta[0];
	for (int i = 1; i < twoTheta.size(); i++) {
		curDif = twoTheta[i] - twoTheta[i - 1];
		if (curDif < minDif)
			minDif = curDif;
		else if (curDif > maxDif)
			maxDif = curDif;
	}

	// Save peaks if data is already processed
	if (((maxDif > 1.1 * minDif) || (maxDif == 0)) && twoTheta.size() < 500) {
		_type = PT_EXP_INT;
		// Talk about what we are doing here
		Output::newline();
		Output::print("Importing an already-processed pattern");

		// Mark the type of pattern 
		_type = PT_EXP_INT;
		_diffractionPeaks.reserve(twoTheta.size());

		for (int i = 0; i < twoTheta.size(); ++i) {
			DiffractionPeak newPeak(twoTheta[i], intensity[i]);
			_diffractionPeaks.push_back(newPeak);
		}

		// Ensure peaks are in sorted order
		std::sort(_diffractionPeaks.begin(), _diffractionPeaks.end());

		// Save min and max two theta
		_minTwoTheta = _diffractionPeaks[0].getAngle() - _resolution;
		_maxTwoTheta = _diffractionPeaks.back().getAngle() + _resolution / 2;
	} else { // Save peaks after processing
		_type = PT_EXP_RAW;
		// Output
		Output::newline();
		Output::print("Processing raw diffraction pattern");
		Output::increase();

		// Store raw pattern
		_continuousTwoTheta.resize(twoTheta.size());
		_continuousIntensity.resize(twoTheta.size());
		for (int i = 0; i < _continuousTwoTheta.size(); i++) {
			_continuousTwoTheta[i] = twoTheta[i];
			_continuousIntensity[i] = intensity[i];
		}

		// Make a copy of the data to process
		vector<double> twoThetaCopy(twoTheta);
		vector<double> intensityCopy(intensity);
		_minTwoTheta = *std::min_element(twoTheta.begin(), twoTheta.end());
		_maxTwoTheta = *std::max_element(twoTheta.begin(), twoTheta.end());

		// Prepare data for peak fitting
		smoothData(twoThetaCopy, intensityCopy);

		if (DIFFRACTION_EXCESSIVE_PRINTING == 1)
			savePattern("xray-smoothed.out", twoThetaCopy, intensityCopy);
		removeBackground(twoThetaCopy, intensityCopy);

		// Get peaks
		vector<vector<double> > peakTwoTheta;
		vector<vector<double> > peakIntensity;
		locatePeaks(peakTwoTheta, peakIntensity, twoThetaCopy, intensityCopy);
		try {
			getPeakIntensities(peakTwoTheta, peakIntensity);
		} catch (int e) {
			_diffractionPeaks.clear();
		}

		// Output
		Output::decrease();
	}
}

/**
 * Given a diffraction pattern, report its match to the pattern stored in this object.
 * @param reference [in] Pattern to match against
 * @return Match between patterns, expressed as an R factor
 */
double Diffraction::rFactor(const Diffraction& reference) {
    // Output
    Output::newline();
    Output::print("Calculating R factor compared to reference pattern");
    Output::increase();

    // Match peaks in this pattern to reference pattern
    matchPeaksToReference(reference);

    // Get the R factor (refining nothing)
    double rFactor = getCurrentRFactor(reference, DR_ABS);

    // Output
    Output::newline();
    Output::print("Optimal R factor of ");
    Output::print(rFactor);

    // Output
    Output::decrease();

    // Return result
    return rFactor;
}

/**
 * Used during structural refinement. Sets positions of atoms.
 *
 * @param symmetry [in,out] Symmetry object describing structure (contains atomic positions)
 * @param positions [in] Vector containing all relevant position data
 */
void CalculatedPattern::setPositions(const Symmetry& symmetry, const Vector& positions) {
    int i, j, k;
    Vector3D newPos;
    for (i = 0; i < symmetry.orbits().length(); ++i) {
        for (j = 0; j < symmetry.orbits()[i].atoms().length(); ++j) {

            for (k = 0; k < 3; ++k)
                newPos[k] = positions[3 * i + k];
            newPos *= symmetry.orbits()[i].generators()[j].rotation();
            newPos += symmetry.orbits()[i].generators()[j].translations()[0];
            ISO::moveIntoCell(newPos);
            symmetry.orbits()[i].atoms()[j]->fractional(newPos);
        }
    }
}

/**
 * Ensure that positions obey symmetry of a crystal.
 * 
 * @param symmetry [in] Object describing the positions
 * @param position [in,out] Derivatives with respect to each position parameter. Will be adjusted
 */
void CalculatedPattern::symPositions(const Symmetry& symmetry, Vector& position) {
    int i, j;
    Vector3D tempPos;
    for (i = 0; i < symmetry.orbits().length(); ++i) {
        for (j = 0; j < 3; ++j)
            tempPos[j] = position[3 * i + j];
        tempPos -= symmetry.orbits()[i].specialPositions()[0].translation();
        tempPos *= symmetry.orbits()[i].specialPositions()[0].rotation();
        tempPos += symmetry.orbits()[i].specialPositions()[0].translation();

        for (j = 0; j < 3; ++j)
            position[3 * i + j] = tempPos[j];
    }
}

/**
 * Set new basis vectors to structure used to generate this pattern
 * @param newParams New parameters: [0-2] lengths, [3-5] angles
 */
void CalculatedPattern::setBasis(vector<double> newParams) {
    Vector3D lengths = Vector3D(newParams[0], newParams[1], newParams[2]);
    Vector3D angles = Vector3D(newParams[3], newParams[4], newParams[5]);
    
    // Compute basis
    Matrix3D basis = Basis::vectors(lengths, angles);
    
    // Refine the basis to match symmetry
    _symmetry->refineBasis(basis);
    
    // Set the structure
    _structure->basis(basis, false);
}


/**
 * Calculate a pattern match where the entire pattern
 * 
 * Ways to calculate R factor:
 * <ul>
 * <li><b>DR_ABS</b>: Is the R<sub>p</sub>, profile reliability factor
 * <li><b>DR_SQUARED</b>: Is the weighted profile residual
 * <li><b>DR_RIETVELD</b>: Unnormalzied, version of DR_SQUARED taken over whole pattern
 * </ul>
 * 
 * See doi:10.1107/S0021889893012348 for a good discussion of rietveld R factors
 * 
 * @param referencePattern [in] Pattern against which data is compared
 * @param rMethod [in] Method used to calculate R factor
 * @return R factor
 */
double CalculatedPattern::getRietveldRFactor(const Diffraction& referencePattern, Rmethod rMethod) {
	// Get reference pattern less the background signal
	vector<double> twoTheta = referencePattern.getMeasurementAngles();
	vector<double> rawRefIntensities = referencePattern.getMeasuredIntensities();
	vector<double> refIntensities; refIntensities.reserve(twoTheta.size());
	vector<double> background = generateBackgroundSignal(twoTheta);
	if (rMethod != DR_RIETVELD) {
		for (int i=0; i<twoTheta.size(); i++) {
			refIntensities.push_back(rawRefIntensities[i] - _optimalScale * background[i]);
		}
	}
	
	// Get computed pattern
	vector<double> thisIntensities = generatePeakSignal(twoTheta);
	
	if (rMethod == DR_ABS) {	
		double num = 0;
		double denom = 0;
		for (int i=0; i<thisIntensities.size(); i++) {
			double refI = refIntensities[i];
			if (refI <= 0) continue; // Don't consider regions outside of background
			num += abs(refI - _optimalScale * thisIntensities[i]);
			denom += refI;
		}
		return denom > 0 ? num / denom : 1;
	} else if (rMethod == DR_SQUARED) {
		vector<double> weight; weight.reserve(twoTheta.size());
		for (int i=0; i<refIntensities.size(); i++) {
			weight.push_back(rawRefIntensities[i] > 0 ? 1.0 / rawRefIntensities[i] : 0.0);
		}
		double denom = 0.0, num = 0.0, diff = 0.0;
		for (int i=0; i<weight.size(); i++) {
			diff = refIntensities[i] - _optimalScale * thisIntensities[i];
			num += weight[i] * diff * diff;
			denom += weight[i] * refIntensities[i] * refIntensities[i];
		}
		return sqrt(num/denom);
	} else if (rMethod == DR_RIETVELD) {
		vector<double> weight; weight.reserve(twoTheta.size());
		for (int i=0; i<rawRefIntensities.size(); i++) {
			weight.push_back(rawRefIntensities[i] > 0 ? 1.0 / rawRefIntensities[i] : 0.0);
		}
		double num = 0.0, diff = 0.0;
		for (int i=0; i<weight.size(); i++) {
			diff = rawRefIntensities[i] - _optimalScale * (thisIntensities[i] + background[i]);
			num += weight[i] * diff * diff;
		}
		return num;
	} else {
		Output::newline(ERROR);
		Output::print("Internal Error: Mint can't calculate a rietveld R factor with that method");
		Output::quit();
		return 0;
	}
}


/**
 * Calculate the current R factor based on the intensities stored in _peaks. This operation
 *  also calculates derivatives of R factor with respect to every variable that can 
 *  be refined. Also automagically determines the optimal scale factor.
 * 
 * User Notices:
 * <ol>
 * <li>You should already have matched peaks to the reference pattern (done separately so that 
 * they do not have be computed each time to call this operation). Be careful!
 * <li>Uses whatever intensities are return when calling getDiffractedPeaks. If you are using a 
 * calculated pattern, make sure to update this before computing the rFactor!
 * </ol>
 * 
 * @param 
 * @param method [in] Method used to calculate R factor
 * @return R factor describing match between this pattern and reference
 */
double Diffraction::getCurrentRFactor(const Diffraction& referencePattern, Rmethod method) {
    if (_matchingPeaks.size() == 0) {
		Output::newline(ERROR);
		Output::print("Some developer forgot to match diffraction peaks first!");
		Output::quit();
	}
	
    // ---> Part #1: Store the peak intensities for both the reference pattern and 
    //   peaks from the calculated pattern that match those peaks
    
    // Intensities of peaks in the reference pattern that are matched by peaks 
    //  in the calculated pattern.
	vector<DiffractionPeak> referencePeaks = referencePattern.getDiffractedPeaks(),
			thisPeaks = getDiffractedPeaks();
	
    vector<double> referenceIntensity(referencePeaks.size(), 0.0);
    // For each peak in the reference, total intensity of all matching peaks from this pattern
    vector<double> matchedIntensity(referenceIntensity.size(), 0.0);
    // Intensity of peaks in this pattern that do not match a reference peak
    vector<double> unmatchedIntensity(_unmatchedPeaks.size());
    
    for (int i = 0; i < referenceIntensity.size(); i++) {
        referenceIntensity[i] = referencePeaks[i].getIntensity();
        for (int j=0; j < _matchingPeaks[i].size(); j++)
            matchedIntensity[i] += thisPeaks[_matchingPeaks[i][j]].getIntensity();
    }
    
    for (int i=0; i<unmatchedIntensity.size(); i++)
        unmatchedIntensity[i] = thisPeaks[_unmatchedPeaks[i]].getIntensity();

    // ---> Part #2: Calculate the normalization factor, which is what the total
    //  error is divided by to make it an R factor. This is generally the sum of
    //  the integrated intensities from the reference pattern
    double norm;
    switch (method) {
        case DR_SQUARED:
            norm = 0;
            for (int i = 0; i < referenceIntensity.size(); i++)
                norm += referenceIntensity[i] * referenceIntensity[i];
            break;
        case DR_ABS:
            norm = std::accumulate(referenceIntensity.begin(), referenceIntensity.end(), 0.0);
            break;
        default:
            Output::newline(ERROR);
            Output::print("Internal Error: No method set to determine normalization factor with this Rmethod.");
            return -1;
    }

    // ---> Part #3: Determine optimal scale factor
    _optimalScale = 1.0;
    if (method == DR_SQUARED) {
        // Very simple for this case. Since R is quadratic wrt scale factor it is 
        //  possible to solve where the first derivative is equal to zero:
        //  s_optimal = sum[ I_ref * I_calc ] / sum[ I_calc * I_calc ]
        // Note: We do not need to worry about where peaks are not matched , because
        //       I_ref * I_calc == 0 for those cases
        _optimalScale = std::inner_product(matchedIntensity.begin(), matchedIntensity.end(), \
                referenceIntensity.begin(), 0.0);
        double denom = std::inner_product(matchedIntensity.begin(), matchedIntensity.end(),
				matchedIntensity.begin(), 0.0);
		denom += std::inner_product(unmatchedIntensity.begin(), unmatchedIntensity.end(),
				unmatchedIntensity.begin(), 0.0);
        _optimalScale /= denom;
    } else if (method == DR_ABS) {
        // In this case, the minimum occurs when at least one calculated peak intensity
        //  is exactly equal to reference peak intensity. So, we are going to loop through
        //  each of these conditions
        double minimumError = 1e100;
        for (int i = 0; i < matchedIntensity.size(); i++) {
            if (matchedIntensity[i] == 0) continue;
                double curScale = referenceIntensity[i] / matchedIntensity[i];
                double curError = 0;
                for (int j = 0; j < matchedIntensity.size(); j++)
                    curError += abs(referenceIntensity[j] - curScale * matchedIntensity[j]);
                for (int j = 0; j < _unmatchedPeaks.size(); j++) 
                    curError += abs(curScale * thisPeaks[_unmatchedPeaks[j]].getIntensity());
                if (curError < minimumError) {
                    minimumError = curError; _optimalScale = curScale; 
                }
        }
    } else {
        Output::newline(ERROR);
        Output::print("Internal Error: No method set to determine optimal scale with this Rmethod.");
        return -1;
    }
    
    // ---> Part #5: Calculate R factor
    double rFactor = 0;
    switch (method) {
        case DR_SQUARED:
            for (int i=0; i < matchedIntensity.size(); i++)
                rFactor += pow(referenceIntensity[i] - _optimalScale * matchedIntensity[i],2);
            for (int i=0; i<unmatchedIntensity.size(); i++)
                rFactor += pow(_optimalScale * unmatchedIntensity[i], 2);
            rFactor /= norm;
            break;
        case DR_ABS:
            for (int i=0; i < matchedIntensity.size(); i++)
                rFactor += abs(referenceIntensity[i] - _optimalScale * matchedIntensity[i]);
            for (int i=0; i<unmatchedIntensity.size(); i++)
                rFactor += abs(_optimalScale * unmatchedIntensity[i]);
            rFactor /= norm;
            break;
        default:
            Output::newline(ERROR);
            Output::print("Internal Error: No method set to determine R factor with this Rmethod.");
            return -1;
    }
    
    return rFactor;
}

/**
 * @param text [in] Text object containing contents of file
 * @return Whether file contains diffraction data
 */
bool ExperimentalPattern::isFormat(const Text& text) {
	int pairCount = 0;
	int lineCount = 0;
	for (int i = 0; i < text.length(); ++i) {
		if (!text[i].length())
			continue;
		if (Language::isComment(text[i][0]))
			continue;
		++lineCount;
		if (text[i].length() < 2)
			continue;
		if ((Language::isNumber(text[i][0])) && (Language::isNumber(text[i][1])))
			++pairCount;
	}
	if (!lineCount)
		return false;
	if ((double) pairCount / lineCount < 0.5)
		return false;
	return true;
}

/**
 * Extract diffraction data from file, store in this object. File must be in the 
 *  following format:
 * <p>wavelength [wavelength]<br>
 * fwhm [fwhm]<br>
 * variance [variance]<br>
 * [twoTheta1] [intensity1]<br>
 * {...}<br>
 * 
 * Wavelength and other text words are optional. As long as the file contains more
 *  at least two columns of numerical data, this functional will read it as a diffraction pattern.
 * 
 * @param text [in] Text object containing contents of a file
 */
void ExperimentalPattern::set(const Text& text) {
    // Output
    Output::newline();
    Output::print("Reading diffraction data from file");
    Output::increase();

    // Clear space
    clear();

    // Loop over lines in file
    int i;
    vector<double> rawTwoTheta;
    vector<double> rawIntensity;

    rawTwoTheta.reserve(text.length());
    rawIntensity.reserve(text.length());
    for (i = 0; i < text.length(); ++i) {

        // Skip if line is blank
        if (!text[i].length())
            continue;

        // Skip if line is too short
        if (text[i].length() < 2)
            continue;

        // Found wavelength
        if (text[i][0].equal("wavelength", false, 4)) {
            if (Language::isNumber(text[i][1]))
                    _wavelength = atof(text[i][1].array());
            else {
                Output::newline(ERROR);
                        Output::print("Did not recognize wavelength value in diffraction file (");
                        Output::print(text[i][1]);
                        Output::print(")");
                        Output::quit();
            }
        }
		
		// Found a data line
        else if ((Language::isNumber(text[i][0])) && (Language::isNumber(text[i][1]))) {
            rawTwoTheta.push_back(atof(text[i][0].array()));
            rawIntensity.push_back(atof(text[i][1].array()));
        }
    }

    // Reduce data arrays to minimal size
    rawTwoTheta.resize(rawTwoTheta.size());
    rawIntensity.resize(rawIntensity.size());

    // Process data
    set(rawTwoTheta, rawIntensity);

    // Output
	if (_diffractionPeaks.size() > 0) {
		vector<DiffractionPeak> peaks = getDiffractedPeaks();
		Output::newline();
		Output::print("Found ");
		Output::print(peaks.size());
		Output::print(" peak");
		if (peaks.size() != 1)
				Output::print("s");
				Output::increase();
		for (i = 0; i < peaks.size(); ++i) {
			Output::newline();
			Output::print("Two-theta and intensity of ");
			Output::print(peaks[i].getAngle());
			Output::print(" ");
			Output::print(peaks[i].getIntensity());
		}
	} else {
		Output::newline(ORDINARY);
		Output::print("Stored a pattern with ");
		Output::print(_continuousIntensity.size());
		Output::print(" measurements.");
	}
	Output::decrease();

    // Output
    Output::decrease();
}

/** 
 * Apply smoothing function to intensities. Works by taking a weighted average of the
 *  value of a certain point and its closest neighbors. 
 * 
 * The weight of the center point is always equal to 1. It is up to the user
 * to determine how many neighbors to include in the smoothing, and the weight 
 * of the point farthest from the center. The weights of the other points is
 * determined through linear interpolation.
 * 
 * @param rawTwoTheta [in] Angles at which intensity is recorded
 * @param rawIntensity [in,out] Intensity measured at each angle. This function
 *  should remove noise from this data.
 * @param numPerSide [in] Number of points to use on either side for smoothing.
 * @param power [in] How much weight to apply to farthest points. Should range between 0 and 1.
 */
void ExperimentalPattern::smoothData(const vector<double>& rawTwoTheta, vector<double>& rawIntensity,
        const int numPerSide, const double power) {

    // Weight for point at max distance away
    double farWeight = power;

	// Calculate weights (linear scaling)
	int numSmoothPoints = numPerSide * 2 + 1;
	double weight[numSmoothPoints];
	weight[numPerSide] = 1.0;
	double totalWeight = 1.0;
    for (int i = 1; i <= numPerSide; i++) {
        double temp = 1.0 + (farWeight - 1.0) * (double) i / (double) numPerSide;
                totalWeight += 2 * temp;
                weight[numPerSide - i ] = temp;
                weight[numPerSide + i ] = temp;
    }
    for (int i = 0; i < numSmoothPoints; i++)
            weight[i] /= totalWeight;

            // Original intensity (before filtering)
            vector<double> initialInt(rawIntensity);

            // Calculate new intensities
        for (int i = numPerSide; i < rawIntensity.size() - numPerSide; i++) {
            double newValue = 0.0;
                    int startPoint = i - numPerSide;

            for (int j = 0; j < numSmoothPoints; j++)
                    newValue += weight[j] * initialInt[startPoint + j];
                    rawIntensity[i] = newValue;
            }
}

/**
 * Given a raw powder diffraction pattern, remove the background signal
 *
 * @param rawTwoTheta [in] Angles at which diffracted intensity is measured
 * @param rawItensity [in,out] Intensity measured at each angle. Background will be 
 *                      removed from these measurements.
 */
void ExperimentalPattern::removeBackground(vector<double>& rawTwoTheta, vector<double>& rawIntensity) {
    // Determine how many points to include in smoothing
    double boxSize = 4.0;
	int nPoints = (int) (boxSize / (rawTwoTheta[1] - rawTwoTheta[0]));
	int pointsPerSide = nPoints / 2;

	// Weight points based on the squared inverse of their intensities
	vector<double> fitWeight(rawIntensity.size());
    for (int i = 0; i < fitWeight.size(); i++) {
        fitWeight[i] = rawIntensity[i] > 0 ? 1.0 / rawIntensity[i] : 10;
		fitWeight[i] *= fitWeight[i]; fitWeight[i] *= fitWeight[i];
    }


    // Determine background signal as weighted average of background 
    vector<double> backgroundSignal(rawIntensity.size(), 0.0);
    for (int point = 0; point < backgroundSignal.size(); point++) {
        double totalWeight = 0.0;
                // Determine how many points to use in average
                int toAverage = min(point, pointsPerSide);
                toAverage = min(toAverage, (int) backgroundSignal.size() - 1 - point);
        for (int neigh = -toAverage; neigh <= toAverage; neigh++) {
            backgroundSignal[point] += fitWeight[point + neigh] * rawIntensity[point + neigh];
                    totalWeight += fitWeight[point + neigh];
        }
        backgroundSignal[point] /= totalWeight;
    }

    // Subtract background
    for (int i = 0; i < rawIntensity.size(); i++)
            rawIntensity[i] -= backgroundSignal[i];


    // If desired, print out background-less signal
    if (DIFFRACTION_EXCESSIVE_PRINTING == 1)
		savePattern("xray-nobackground.out", rawTwoTheta, rawIntensity, backgroundSignal);
}



/**
 * Detects diffraction peaks from raw diffraction pattern data.
 * 
 * @param peakTwoTheta [out] 2D list. For each detected peak, contains list of angles corresponding to measured peak
 * @param peakIntensity [out] 2D list. For each detected peak, contains intensities measured at each angle
 * @param rawTwoTheta [in] Raw diffraction pattern: Angles at which intensities were measured
 * @param rawIntensity [in] Raw diffraction pattern: Intensities measured at each angle
 */
void ExperimentalPattern::locatePeaks(vector<vector<double> >& peakTwoTheta,
        vector<vector<double> >& peakIntensity,
        const vector<double>& rawTwoTheta, const vector<double>& rawIntensity) {

    // Tolerance for point being on a peak. Fraction of maximum intensity
    double peakTol = 0.01;

    // Clear space for results
    peakTwoTheta.clear();
    peakIntensity.clear();

    // Loop over points to get maximum
    double maxHeight = *std::max_element(rawIntensity.begin(), rawIntensity.end());

    // Set absolute peak tolerance
    peakTol *= maxHeight;

    // First derivative of intensity wrt twoTheta
    vector<double> firstDerivative = getFirstDerivative(rawTwoTheta, rawIntensity);
    smoothData(rawTwoTheta, firstDerivative, 3, 1.0);
    if (DIFFRACTION_EXCESSIVE_PRINTING == 1)
    savePattern("xray-firstDerivative.out", rawTwoTheta, firstDerivative);

    // Second derivative of intensity wrt twoTheta
    vector<double> secondDerivative = getSecondDerivative(rawTwoTheta, rawIntensity);
    smoothData(rawTwoTheta, secondDerivative, 3, 1.0);
    if (DIFFRACTION_EXCESSIVE_PRINTING == 1)
        savePattern("xray-secondDerivative.out", rawTwoTheta, secondDerivative);
            
    // Position of center of peaks
    int position = 0;
    // Peak positions (stored as index in each array)
    vector<int> peakPosition;
    // Loop through entire pattern
    while (position < rawTwoTheta.size()) {
        // Loop until we find intensities that are above peak tolerance and a positive 
        //  second derivative
        while (rawIntensity[position] < peakTol || secondDerivative[position] < 0) {
            position++; if (position == rawTwoTheta.size()) break;
            }
        // See if we are done searching
        if (position == rawTwoTheta.size()) break;

                // Loop until second derivatives crosses zero
            while (secondDerivative[position] > 0) {
                position++; if (position == rawTwoTheta.size()) break;
                }
        if (position == rawTwoTheta.size()) break;

                // Loop until first derivative crosses zero (marks center)
            while (firstDerivative[position] > 0) {
                position++; if (position == rawTwoTheta.size()) break;
                }
        if (position == rawTwoTheta.size()) break;
                peakPosition.push_back(position);

                // Loop until end of peak (second derivative goes positive)
            while (secondDerivative[position] < 0) {
                position++; if (position == rawTwoTheta.size()) break;
                }
        if (position == rawTwoTheta.size()) { // Peak did not finish
            peakPosition.pop_back(); break;
        }
    }

    // Part 2: Store edges of peaks
    peakIntensity.clear();
    peakIntensity.reserve(peakPosition.size());
    peakTwoTheta.clear();
    peakTwoTheta.reserve(peakPosition.size());

    // Lower edge of peak
    int leftMinimum = 0;
    double temp = rawIntensity[0];
    for (int i = 1; i < peakPosition[0]; i++) {
        if (rawIntensity[i] < temp) {
            temp = rawIntensity[i]; leftMinimum = i;
        }
    }
    // Upper edge of peak
    int rightMinimum = 0;
    for (int i = 0; i < peakPosition.size(); i++) {
        // Step 1: Find the minimum between next peak and current peak
        temp = rawIntensity[peakPosition[i]];
                rightMinimum = peakPosition[i];
                double rightMaximum = i == peakPosition.size() - 1 ? rawIntensity.size()
                : peakPosition[i + 1];
        for (position = peakPosition[i]; position < rightMaximum; position++) {
            if (rawIntensity[position] < temp) {
                temp = rawIntensity[position]; rightMinimum = position; }
        }
        // Step 2: Define edges of peak (either the minimum between peaks, 
        //         or where intensity crosses zero). Store in a deque temporarily
        deque<double> dequePeakTwoTheta, dequePeakIntensity;
                position = peakPosition[i];
        while (position >= leftMinimum && rawIntensity[position] > 0) {
            dequePeakTwoTheta.push_front(rawTwoTheta[position]);
                    dequePeakIntensity.push_front(rawIntensity[position]);
                    position--;
        }
        position = peakPosition[i] + 1;
        while (position <= rightMinimum && rawIntensity[position] > 0) {
            dequePeakTwoTheta.push_back(rawTwoTheta[position]);
                    dequePeakIntensity.push_back(rawIntensity[position]);
                    position++;
        }
        // Step 3: Prepare to move to next peak
        // Store current peak
        vector<double> tempVector(dequePeakTwoTheta.begin(),
                dequePeakTwoTheta.end());
        if (tempVector.size() > 0) {
            peakTwoTheta.push_back(tempVector);
                    tempVector.assign(dequePeakIntensity.begin(), dequePeakIntensity.end());
                    peakIntensity.push_back(tempVector);
        }
        // Update boundary
        leftMinimum = rightMinimum;
    }

    // Part 3: Combine peaks that are smaller than certain criteria
    position = 0;
    while (position < peakTwoTheta.size()) {
        bool toRemove = false;
                // Step 1: Check maximum intensity of peak is above 2% of the maximum
                double peakHeight = *max_element(peakIntensity[position].begin(), \
                    peakIntensity[position].end());
                toRemove = peakHeight < 0.02 * maxHeight;
                // Step 2: If tall enough, check if peak width is greater than 0.1 degrees
                double peakWidth = peakTwoTheta[position].back() - peakTwoTheta[position][0];
        if (!toRemove) toRemove = peakWidth < 0.05;
                // Step 3: If it fails either test, remove this peak.
            if (toRemove) {
                // First, check if peak on right is connected
                if (position != peakTwoTheta.size() - 1 && \
                        peakTwoTheta[position].back() == peakTwoTheta[position + 1][0]) {
                    // If so, add this peak to that one
                    peakTwoTheta[position + 1].insert(peakTwoTheta[position + 1].begin(), \
                            peakTwoTheta[position].begin(), peakTwoTheta[position].end());
                            peakIntensity[position + 1].insert(peakIntensity[position + 1].begin(), \
                            peakIntensity[position].begin(), peakIntensity[position].end());
                } else if (position != 0 && \
                        peakTwoTheta[position][0] == peakTwoTheta[position - 1].back()) {
                    peakTwoTheta[position - 1].insert(peakTwoTheta[position - 1].end(), \
                            peakTwoTheta[position].begin(), peakTwoTheta[position].end());
                            peakIntensity[position - 1].insert(peakIntensity[position - 1].end(), \
                            peakIntensity[position].begin(), peakIntensity[position].end());
                }
                // Now delete, the peak
                peakTwoTheta.erase(peakTwoTheta.begin() + position);
                        peakIntensity.erase(peakIntensity.begin() + position);
            }

              else position++;
            }
    Output::decrease();
}

/** 
 * Given a list of peaks extracted from a raw diffraction pattern, fit interpolation functions to 
 *  each peak and use them to calculate area under peak. Locations and integrated intensities
 *  of each peak are stored internally in _twoTheta and _intensity.
 * 
 * The two inputs to this function are created by running a background-subtracted, 
 *  smoothed raw diffraction pattern through getPeaks().
 * 
 * @param peakTwoTheta [in] 2D array containing angles corresponding to each peak. Peaks must 
 *  be arranged in ascending order with two theta.
 * @param peakIntensity [in] 2D array containing intensities at each measured angle for each peak
 */
void ExperimentalPattern::getPeakIntensities(const vector<vector<double> >& peakTwoTheta, \
        const vector<vector<double> >& peakIntensity) {
    // Clear the old peak data out
    _diffractionPeaks.clear();
    _diffractionPeaks.reserve(peakIntensity.size());
    
    // Functors used in fitting
    Functor<ExperimentalPattern> gaussFun(this, &ExperimentalPattern::gaussian);
    Functor<ExperimentalPattern> compositeGaussFun(this, &ExperimentalPattern::compositeGaussian);
    Functor<ExperimentalPattern> psFun(this, &ExperimentalPattern::PV);
    Functor<ExperimentalPattern> compositePVFun(this, &ExperimentalPattern::compositePV);
    VectorFunctor<ExperimentalPattern> gaussDeriv(this, &ExperimentalPattern::gaussianDerivs);
    VectorFunctor<ExperimentalPattern> compositeGaussDeriv(this, &ExperimentalPattern::compositeGaussianDerivs);
    VectorFunctor<ExperimentalPattern> psDeriv(this, &ExperimentalPattern::PVderivs);
    VectorFunctor<ExperimentalPattern> compositePVDeriv(this, &ExperimentalPattern::compositePVDerivs);


    // Part #0: Save two-theta/intensity pairs in a format compatable with 
    //  Mint's fitting programs
    List<double>::D3 singlePeakPoints(peakTwoTheta.size());
    for (int curPeak = 0; curPeak < peakTwoTheta.size(); curPeak++) {
        singlePeakPoints[curPeak].length(peakTwoTheta[curPeak].size());
                vector<double> curPeakTwoTheta = peakTwoTheta[curPeak];
                vector<double> curPeakIntensity = peakIntensity[curPeak];
        for (int j = 0; j < peakTwoTheta[curPeak].size(); ++j) {
            singlePeakPoints[curPeak][j].length(2);
                    singlePeakPoints[curPeak][j][0] = curPeakTwoTheta[j];
                    singlePeakPoints[curPeak][j][1] = curPeakIntensity[j];
        }
    }

    // Part #1: Fit individual peaks with Gaussian functions
    OList<Vector> gaussianParams(peakTwoTheta.size());
    for (int curPeak = 0; curPeak < peakTwoTheta.size(); ++curPeak) {
        // Initialize fitting parameters for Gaussian function
        Vector initialGaussian(3);
                initialGaussian[0] = 0.25;
                initialGaussian[1] = singlePeakPoints[curPeak][0][0];
                initialGaussian[2] = singlePeakPoints[curPeak][0][1];
        for (int j = 1; j < singlePeakPoints[curPeak].length(); ++j) {
            if (singlePeakPoints[curPeak][j][1] > initialGaussian[2]) {
                initialGaussian[1] = singlePeakPoints[curPeak][j][0];
                        initialGaussian[2] = singlePeakPoints[curPeak][j][1];
            }
        }

        // Fit Gaussian function
        gaussianParams[curPeak] = Fit::LM<ExperimentalPattern>(singlePeakPoints[curPeak], gaussFun, gaussDeriv, initialGaussian, 1e-5);
    }

    // Part #2: Decide which peaks need to be fitted together
    // Detect which peaks are in contact
    vector<vector<int> > peakGroup; peakGroup.reserve(peakTwoTheta.size());
    {
        vector<int> newVec; newVec.push_back(0); peakGroup.push_back(newVec); 
	}
	for (int peak = 1; peak < peakTwoTheta.size(); peak++) {
		double peakStart = peakTwoTheta[peak].front();
		double lastGroupEnd = peakTwoTheta[peakGroup.back().back()].back();
		if (peakStart - lastGroupEnd < 0.1)
			peakGroup.back().push_back(peak);
		else {
			vector<int> newGroup(1, peak);
			peakGroup.push_back(newGroup);
		}
    }
    // Combine data from peak groups
	List<double>::D3 peakGroupPoints(peakGroup.size());
	for (int group = 0; group < peakGroup.size(); group++) {
		int totalPeakSize = 0;
		for (int subPeak = 0; subPeak < peakGroup[group].size(); subPeak++)
			totalPeakSize += singlePeakPoints[peakGroup[group][subPeak]].length();
		// Copying points from each singlePeak (in reverse order)
		peakGroupPoints[group].length(totalPeakSize);
		for (int subPeak = peakGroup[group].size() - 1; subPeak >= 0; subPeak--) {
			int curPeak = peakGroup[group][subPeak];
			for (int point = singlePeakPoints[curPeak].length() - 1; point >= 0; point--)
				peakGroupPoints[group][--totalPeakSize] = singlePeakPoints[curPeak][point];
		}
	}

    // Part #3: Fit groups of peaks with multiple Gaussian functions. Note
    //  that each Gaussian still corresponds to a single peak.
	for (int group = 0; group < peakGroup.size(); group++) {
		// If only a single peak in group, do nothing
		if (peakGroup[group].size() == 1) continue;
		// Step #1: Extract parameters from individual Gaussians
		Vector initialCompositeParams(3 * peakGroup[group].size());
		for (int peak = 0; peak < peakGroup[group].size(); peak++)
			for (int i = 0; i < 3; i++)
				initialCompositeParams[peak * 3 + i] = gaussianParams[peakGroup[group][peak]][i];
		// Step #2: Fit new parameters 
		Vector compositeParams = Fit::LM<ExperimentalPattern>(peakGroupPoints[group],
				compositeGaussFun, compositeGaussDeriv, initialCompositeParams, 1e-5);
		// Step #3: Copy parameters back
		for (int peak = 0; peak < peakGroup[group].size(); peak++)
			for (int i = 0; i < 3; i++)
				gaussianParams[peakGroup[group][peak]][i] = compositeParams[peak * 3 + i];
	}

    // Part #4: Fit groups of peaks with multiple pseudo-Voight functions
    OList<Vector> psParams(peakTwoTheta.size());
            // Step #1: Convert Guassian results into inital pseudo-Voight guesses
    for (int curPeak = 0; curPeak < peakTwoTheta.size(); curPeak++) {
        Vector initialPS(8);
        // Initialize pseudo-voigt fitting parameters using solution from Gaussian
        initialPS[0] = 1.0; // Purely weight on the Guassian
        initialPS[1] = initialPS[2] = 0.0;
        initialPS[3] = gaussianParams[curPeak][1];
        // double temp = tan(Num<double>::toRadians(initialPS[3]/2));
        // initialPS[4] = initialPS[5] = initialPS[6] = gaussianParams[curPeak][0] / (1 + temp + temp*temp);
        initialPS[4] = gaussianParams[curPeak][0];
        initialPS[5] = initialPS[6] = 0.0;
        initialPS[7] = gaussianParams[curPeak][2];

        // Store result
        psParams[curPeak] = initialPS;
	}

	for (int group = 0; group < peakGroup.size(); group++) {
		// Step #2: Extract parameters from individual peaks
		Vector initialCompositeParams(8 * peakGroup[group].size());
		for (int peak = 0; peak < peakGroup[group].size(); peak++)
			for (int i = 0; i < 8; i++)
				initialCompositeParams[peak * 8 + i] = psParams[peakGroup[group][peak]][i];
		// Step #3: Fit new parameters 
		Vector compositeParams = Fit::LM<ExperimentalPattern>(peakGroupPoints[group],
				compositePVFun, compositePVDeriv, initialCompositeParams, 1e-5);
		// Step #4: Copy parameters back
		for (int peak = 0; peak < peakGroup[group].size(); peak++)
			for (int i = 0; i < 8; i++)
				psParams[peakGroup[group][peak]][i] = compositeParams[peak * 8 + i];
	}

    // Part #5: Extract peak intensities, store in _diffractionPeaks
    Output::increase();
    for (int group = 0; group < peakGroup.size(); group++) {
        double groupMin = peakTwoTheta[peakGroup[group].front()].front();
        double groupMax = peakTwoTheta[peakGroup[group].back()].back();
        for (int subPeak = 0; subPeak < peakGroup[group].size(); subPeak++) {
            int curPeak = peakGroup[group][subPeak];
            double initialTwoTheta = psParams[curPeak][3];
            double twoThetaStep = 1e-3;
            Functor<ExperimentalPattern> psTT(this, &ExperimentalPattern::PV, &psParams[curPeak]);
            double location, intensity;
            
            // Find the maximum (location of peak)
            intensity = Solve<ExperimentalPattern>::maximize(psTT, 1e-8, initialTwoTheta, twoThetaStep, location);
            
            // Integrate the peak
            PVPeakFunction peakFunc(this, psParams[curPeak]);
            intensity = dlib::integrate_function_adapt_simp(peakFunc, groupMin, groupMax, 1e-8);
			
			// Check that results make sense
			if (intensity < 0.0) {
				Output::newline(WARNING);
				Output::print("Failure during peak integration - Negative intensity found near: ");
				Output::print(location, 3);
				Output::decrease();
				throw 10;
			}
			
			// Check that the maximum is within bounds of the measurement
			if (location < this->_minTwoTheta || location > this->_maxTwoTheta) {
				Output::newline(WARNING);
				Output::print("Failure during peak integration - Peak maximum outside of measured range: ");
				Output::print(location, 3);
				Output::decrease();
				throw 10;
			}
            
            // Make a new peak
            DiffractionPeak newPeak(location, intensity);
            _diffractionPeaks.push_back(newPeak);
        }
    }
    Output::decrease();

    // Now that we are done, print out all the data
    if (DIFFRACTION_EXCESSIVE_PRINTING == 1) {
        for (int peak = 0; peak < peakTwoTheta.size(); peak++) {
            vector<double> fittedIntensity(peakTwoTheta[peak].size());
            for (int pos = 0; pos < peakTwoTheta[peak].size(); pos++)
                    fittedIntensity[pos] = PV(psParams[peak], peakTwoTheta[peak][pos]);
                    //fittedIntensity[pos] = gaussian(gaussianParams[peak], peakTwoTheta[peak][pos]);
                    Word filename = "peaks/peak";
                    filename += Language::numberToWord(peak) + Word(".out");
                    savePattern(filename, peakTwoTheta[peak], peakIntensity[peak], fittedIntensity);
            }
    }
}



/**
 * Print diffraction data stored in this object to a file.
 * 
 * @param file [in] Name of file to receive output. Can be "stdout"
 * @param continuous [in] Whether to print the diffraction pattern as a continuous function (true),
 *    or only the peak centers and integrated intensities (false).
 */
void Diffraction::print(const Word& file, bool continuous) const {

    // Open file for writing if needed
    int origStream = Output::streamID();
	PrintMethod origMethod = Output::method();
    if (file != "stdout")
            Output::setStream(Output::addStream(file));

    // Set output method
    Output::method(STANDARD);

    // If printing to file, then print settings
    Output message;
    if (file != "stdout") {
        Output::newline();
        Output::print("Wavelength ");
        Output::print(_wavelength);
        Output::newline();
        Output::print("Resolution ");
        Output::print(_resolution);
    }
    // If printing to screen then add header
    else {
        message.addLine();
        message.add("Two-theta");
        message.add("Intensity");
        message.addLine();
        message.add("---------");
        message.add("---------");
    }

    // Add peaks
    if (!continuous) {
		vector<DiffractionPeak> peaks = getDiffractedPeaks();
        message.addLines(peaks.size());
        for (int i = 0; i < peaks.size(); ++i) {
            if (peaks[i].getIntensity() * _optimalScale < 1) 
                continue;
            message.addLine();
            message.add(peaks[i].getAngle(), 10);
            message.add(peaks[i].getIntensity() * _optimalScale, 10);
        }
    }
    // Add broadened data
    else {
        vector<double> twoTheta = getMeasurementAngles();
		vector<double> intensity = getDiffractedIntensity(twoTheta);
        message.addLines(intensity.size());
        for (int i=0; i < intensity.size(); i++) {
            message.addLine();
            message.add(twoTheta[i], 10);
            message.add(intensity[i] * _optimalScale, 10);
        }
    }

    // Print peaks
    Output::newline();
    Output::print(message, RIGHT);

    // Reset output
    if (file != "stdout")
        Output::removeStream(Output::streamID());
    Output::setStream(origStream);
    Output::method(origMethod);
}

/**
 * Internally store parameters used for atomic form factor (used to calculated atomic scattering
 * factor) . This function simply gets the atomic form factors for each symmetrically
 * unique set of atoms in the structure (should be already defined)
 */
void CalculatedPattern::setATFParams() {
    if (!structureIsDefined()) {
        Output::newline(ERROR);
                Output::print("Structure has not yet been defined. Cannot get ATF parameters");
    }
    // Allocate space
    _atfParams.length(_symmetry->orbits().length());

            // Get parameters
            double a1, a2, a3, a4, b1, b2, b3, b4, c;
    for (int i = 0; i < _symmetry->orbits().length(); ++i) {
        const Element& element = _symmetry->orbits()[i].atoms()[0]->element();
        if (element.number() == 1) {
            a1 = 0.489918; b1 = 20.659300; a2 = 0.262003; b2 = 7.740390; a3 = 0.196767; b3 = 49.551899;
                    a4 = 0.049879; b4 = 2.201590; c = 0.001305;
        } else if (element.number() == 2) {
            a1 = 0.873400; b1 = 9.103700; a2 = 0.630900; b2 = 3.356800; a3 = 0.311200; b3 = 22.927601;
                    a4 = 0.178000; b4 = 0.982100; c = 0.006400;
        } else if (element.number() == 3) {
            a1 = 1.128200; b1 = 3.954600; a2 = 0.750800; b2 = 1.052400; a3 = 0.617500; b3 = 85.390503;
                    a4 = 0.465300; b4 = 168.261002; c = 0.037700;
        } else if (element.number() == 4) {
            a1 = 1.591900; b1 = 43.642700; a2 = 1.127800; b2 = 1.862300; a3 = 0.539100; b3 = 103.483002;
                    a4 = 0.702900; b4 = 0.542000; c = 0.038500;
        } else if (element.number() == 5) {
            a1 = 2.054500; b1 = 23.218500; a2 = 1.332600; b2 = 1.021000; a3 = 1.097900; b3 = 60.349800;
                    a4 = 0.706800; b4 = 0.140300; c = -0.193200;
        } else if (element.number() == 6) {
            a1 = 2.310000; b1 = 20.843901; a2 = 1.020000; b2 = 10.207500; a3 = 1.588600; b3 = 0.568700;
                    a4 = 0.865000; b4 = 51.651199; c = 0.215600;
        } else if (element.number() == 7) {
            a1 = 12.212600; b1 = 0.005700; a2 = 3.132200; b2 = 9.893300; a3 = 2.012500; b3 = 28.997499;
                    a4 = 1.166300; b4 = 0.582600; c = -11.529000;
        } else if (element.number() == 8) {
            a1 = 3.048500; b1 = 13.277100; a2 = 2.286800; b2 = 5.701100; a3 = 1.546300; b3 = 0.323900;
                    a4 = 0.867000; b4 = 32.908901; c = 0.250800;
        } else if (element.number() == 9) {
            a1 = 3.539200; b1 = 10.282500; a2 = 2.641200; b2 = 4.294400; a3 = 1.517000; b3 = 0.261500;
                    a4 = 1.024300; b4 = 26.147600; c = 0.277600;
        } else if (element.number() == 10) {
            a1 = 3.955300; b1 = 8.404200; a2 = 3.112500; b2 = 3.426200; a3 = 1.454600; b3 = 0.230600;
                    a4 = 1.125100; b4 = 21.718399; c = 0.351500;
        } else if (element.number() == 11) {
            a1 = 4.762600; b1 = 3.285000; a2 = 3.173600; b2 = 8.842200; a3 = 1.267400; b3 = 0.313600;
                    a4 = 1.112800; b4 = 129.423996; c = 0.676000;
        } else if (element.number() == 12) {
            a1 = 5.420400; b1 = 2.827500; a2 = 2.173500; b2 = 79.261101; a3 = 1.226900; b3 = 0.380800;
                    a4 = 2.307300; b4 = 7.193700; c = 0.858400;
        } else if (element.number() == 13) {
            a1 = 6.420200; b1 = 3.038700; a2 = 1.900200; b2 = 0.742600; a3 = 1.593600; b3 = 31.547199;
                    a4 = 1.964600; b4 = 85.088600; c = 1.115100;
        } else if (element.number() == 14) {
            a1 = 6.291500; b1 = 2.438600; a2 = 3.035300; b2 = 32.333698; a3 = 1.989100; b3 = 0.678500;
                    a4 = 1.541000; b4 = 81.693703; c = 1.140700;
        } else if (element.number() == 15) {
            a1 = 6.434500; b1 = 1.906700; a2 = 4.179100; b2 = 27.157000; a3 = 1.780000; b3 = 0.526000;
                    a4 = 1.490800; b4 = 68.164497; c = 1.114900;
        } else if (element.number() == 16) {
            a1 = 6.905300; b1 = 1.467900; a2 = 5.203400; b2 = 22.215099; a3 = 1.437900; b3 = 0.253600;
                    a4 = 1.586300; b4 = 56.172001; c = 0.866900;
        } else if (element.number() == 17) {
            a1 = 11.460400; b1 = 0.010400; a2 = 7.196400; b2 = 1.166200; a3 = 6.255600; b3 = 18.519400;
                    a4 = 1.645500; b4 = 47.778400; c = -9.557400;
        } else if (element.number() == 18) {
            a1 = 7.484500; b1 = 0.907200; a2 = 6.772300; b2 = 14.840700; a3 = 0.653900; b3 = 43.898300;
                    a4 = 1.644200; b4 = 33.392899; c = 1.444500;
        } else if (element.number() == 19) {
            a1 = 8.218600; b1 = 12.794900; a2 = 7.439800; b2 = 0.774800; a3 = 1.051900; b3 = 213.186996;
                    a4 = 0.865900; b4 = 41.684101; c = 1.422800;
        } else if (element.number() == 20) {
            a1 = 8.626600; b1 = 10.442100; a2 = 7.387300; b2 = 0.659900; a3 = 1.589900; b3 = 85.748398;
                    a4 = 1.021100; b4 = 178.436996; c = 1.375100;
        } else if (element.number() == 21) {
            a1 = 9.189000; b1 = 9.021300; a2 = 7.367900; b2 = 0.572900; a3 = 1.640900; b3 = 136.108002;
                    a4 = 1.468000; b4 = 51.353100; c = 1.332900;
        } else if (element.number() == 22) {
            a1 = 9.759500; b1 = 7.850800; a2 = 7.355800; b2 = 0.500000; a3 = 1.699100; b3 = 35.633801;
                    a4 = 1.902100; b4 = 116.105003; c = 1.280700;
        } else if (element.number() == 23) {
            a1 = 10.297100; b1 = 6.865700; a2 = 7.351100; b2 = 0.438500; a3 = 2.070300; b3 = 26.893801;
                    a4 = 2.057100; b4 = 102.477997; c = 1.219900;
        } else if (element.number() == 24) {
            a1 = 10.640600; b1 = 6.103800; a2 = 7.353700; b2 = 0.392000; a3 = 3.324000; b3 = 20.262600;
                    a4 = 1.492200; b4 = 98.739899; c = 1.183200;
        } else if (element.number() == 25) {
            a1 = 11.281900; b1 = 5.340900; a2 = 7.357300; b2 = 0.343200; a3 = 3.019300; b3 = 17.867399;
                    a4 = 2.244100; b4 = 83.754303; c = 1.089600;
        } else if (element.number() == 26) {
            a1 = 11.769500; b1 = 4.761100; a2 = 7.357300; b2 = 0.307200; a3 = 3.522200; b3 = 15.353500;
                    a4 = 2.304500; b4 = 76.880501; c = 1.036900;
        } else if (element.number() == 27) {
            a1 = 12.284100; b1 = 4.279100; a2 = 7.340900; b2 = 0.278400; a3 = 4.003400; b3 = 13.535900;
                    a4 = 2.348800; b4 = 71.169197; c = 1.011800;
        } else if (element.number() == 28) {
            a1 = 12.837600; b1 = 3.878500; a2 = 7.292000; b2 = 0.256500; a3 = 4.443800; b3 = 12.176300;
                    a4 = 2.380000; b4 = 66.342102; c = 1.034100;
        } else if (element.number() == 29) {
            a1 = 13.338000; b1 = 3.582800; a2 = 7.167600; b2 = 0.247000; a3 = 5.615800; b3 = 11.396600;
                    a4 = 1.673500; b4 = 64.812599; c = 1.191000;
        } else if (element.number() == 30) {
            a1 = 14.074300; b1 = 3.265500; a2 = 7.031800; b2 = 0.233300; a3 = 5.165200; b3 = 10.316300;
                    a4 = 2.410000; b4 = 58.709702; c = 1.304100;
        } else if (element.number() == 31) {
            a1 = 15.235400; b1 = 3.066900; a2 = 6.700600; b2 = 0.241200; a3 = 4.359100; b3 = 10.780500;
                    a4 = 2.962300; b4 = 61.413502; c = 1.718900;
        } else if (element.number() == 32) {
            a1 = 16.081600; b1 = 2.850900; a2 = 6.374700; b2 = 0.251600; a3 = 3.706800; b3 = 11.446800;
                    a4 = 3.683000; b4 = 54.762501; c = 2.131300;
        } else if (element.number() == 33) {
            a1 = 10.672300; b1 = 2.634500; a2 = 6.070100; b2 = 0.264700; a3 = 3.431300; b3 = 12.947900;
                    a4 = 4.277900; b4 = 47.797199; c = 2.531000;
        } else if (element.number() == 34) {
            a1 = 17.000601; b1 = 2.409800; a2 = 5.819600; b2 = 0.272600; a3 = 3.973100; b3 = 15.237200;
                    a4 = 4.354300; b4 = 43.816299; c = 2.840900;
        } else if (element.number() == 35) {
            a1 = 17.178900; b1 = 2.172300; a2 = 5.235800; b2 = 16.579599; a3 = 5.637700; b3 = 0.260900;
                    a4 = 3.985100; b4 = 41.432800; c = 2.955700;
        } else if (element.number() == 36) {
            a1 = 17.355499; b1 = 1.938400; a2 = 6.728600; b2 = 16.562300; a3 = 5.549300; b3 = 0.226100;
                    a4 = 3.537500; b4 = 39.397202; c = 2.825000;
        } else if (element.number() == 37) {
            a1 = 17.178400; b1 = 1.788800; a2 = 9.643500; b2 = 17.315100; a3 = 5.139900; b3 = 0.274800;
                    a4 = 1.529200; b4 = 164.934006; c = 3.487300;
        } else if (element.number() == 38) {
            a1 = 17.566299; b1 = 1.556400; a2 = 9.818400; b2 = 14.098800; a3 = 5.422000; b3 = 0.166400;
                    a4 = 2.669400; b4 = 132.376007; c = 2.506400;
        } else if (element.number() == 39) {
            a1 = 17.775999; b1 = 1.402900; a2 = 10.294600; b2 = 12.800600; a3 = 5.726290; b3 = 0.125599;
                    a4 = 3.265880; b4 = 104.353996; c = 1.912130;
        } else if (element.number() == 40) {
            a1 = 17.876499; b1 = 1.276180; a2 = 10.948000; b2 = 11.916000; a3 = 5.417320; b3 = 0.117622;
                    a4 = 3.657210; b4 = 87.662697; c = 2.069290;
        } else if (element.number() == 41) {
            a1 = 17.614201; b1 = 1.188650; a2 = 12.014400; b2 = 11.766000; a3 = 4.041830; b3 = 0.204785;
                    a4 = 3.533460; b4 = 69.795700; c = 3.755910;
        } else if (element.number() == 42) {
            a1 = 3.702500; b1 = 0.277200; a2 = 17.235600; b2 = 1.095800; a3 = 12.887600; b3 = 11.004000;
                    a4 = 3.742900; b4 = 61.658401; c = 4.387500;
        } else if (element.number() == 43) {
            a1 = 19.130100; b1 = 0.864132; a2 = 11.094800; b2 = 8.144870; a3 = 4.649010; b3 = 21.570700;
                    a4 = 2.712630; b4 = 86.847198; c = 5.404280;
        } else if (element.number() == 44) {
            a1 = 19.267401; b1 = 0.808520; a2 = 12.918200; b2 = 8.434670; a3 = 4.863370; b3 = 24.799700;
                    a4 = 1.567560; b4 = 94.292801; c = 5.378740;
        } else if (element.number() == 45) {
            a1 = 19.295700; b1 = 0.751536; a2 = 14.350100; b2 = 8.217580; a3 = 4.734250; b3 = 25.874901;
                    a4 = 1.289180; b4 = 98.606201; c = 5.328000;
        } else if (element.number() == 46) {
            a1 = 19.331900; b1 = 0.698655; a2 = 15.501700; b2 = 7.989290; a3 = 5.295370; b3 = 25.205200;
                    a4 = 0.605844; b4 = 76.898598; c = 5.265930;
        } else if (element.number() == 47) {
            a1 = 19.280800; b1 = 0.644600; a2 = 16.688499; b2 = 7.472600; a3 = 4.804500; b3 = 24.660500;
                    a4 = 1.046300; b4 = 99.815598; c = 5.179000;
        } else if (element.number() == 48) {
            a1 = 19.221399; b1 = 0.594600; a2 = 17.644400; b2 = 6.908900; a3 = 4.461000; b3 = 24.700800;
                    a4 = 1.602900; b4 = 87.482498; c = 5.069400;
        } else if (element.number() == 49) {
            a1 = 19.162399; b1 = 0.547600; a2 = 18.559601; b2 = 6.377600; a3 = 4.294800; b3 = 25.849899;
                    a4 = 2.039600; b4 = 92.802902; c = 4.939100;
        } else if (element.number() == 50) {
            a1 = 19.188900; b1 = 5.830300; a2 = 19.100500; b2 = 0.503100; a3 = 4.458500; b3 = 26.890900;
                    a4 = 2.466300; b4 = 83.957100; c = 4.782100;
        } else if (element.number() == 51) {
            a1 = 19.641800; b1 = 5.303400; a2 = 19.045500; b2 = 0.460700; a3 = 5.037100; b3 = 27.907400;
                    a4 = 2.682700; b4 = 75.282501; c = 4.590900;
        } else if (element.number() == 52) {
            a1 = 19.964399; b1 = 4.817420; a2 = 19.013800; b2 = 0.420885; a3 = 6.144870; b3 = 28.528400;
                    a4 = 2.523900; b4 = 70.840302; c = 4.352000;
        } else if (element.number() == 53) {
            a1 = 20.147200; b1 = 4.347000; a2 = 18.994900; b2 = 0.381400; a3 = 7.513800; b3 = 27.766001;
                    a4 = 2.273500; b4 = 66.877602; c = 4.071200;
        } else if (element.number() == 54) {
            a1 = 20.293301; b1 = 3.928200; a2 = 19.029800; b2 = 0.344000; a3 = 8.976700; b3 = 26.465900;
                    a4 = 1.990000; b4 = 64.265800; c = 3.711800;
        } else if (element.number() == 55) {
            a1 = 20.389200; b1 = 3.569000; a2 = 19.106199; b2 = 0.310700; a3 = 10.662000; b3 = 24.387899;
                    a4 = 1.495300; b4 = 213.904007; c = 3.335200;
        } else if (element.number() == 56) {
            a1 = 20.336100; b1 = 3.216000; a2 = 19.297001; b2 = 0.275600; a3 = 10.888000; b3 = 20.207300;
                    a4 = 2.695900; b4 = 167.201996; c = 2.773100;
        } else if (element.number() == 57) {
            a1 = 20.577999; b1 = 2.948170; a2 = 19.599001; b2 = 0.244475; a3 = 11.372700; b3 = 18.772600;
                    a4 = 3.287190; b4 = 133.123993; c = 2.146780;
        } else if (element.number() == 58) {
            a1 = 21.167101; b1 = 2.812190; a2 = 19.769501; b2 = 0.226836; a3 = 11.851300; b3 = 17.608299;
                    a4 = 3.330490; b4 = 127.112999; c = 1.862640;
        } else if (element.number() == 59) {
            a1 = 22.044001; b1 = 2.773930; a2 = 19.669701; b2 = 0.222087; a3 = 12.385600; b3 = 16.766899;
                    a4 = 2.824280; b4 = 143.643997; c = 2.058300;
        } else if (element.number() == 60) {
            a1 = 22.684500; b1 = 2.662480; a2 = 19.684700; b2 = 0.210628; a3 = 12.774000; b3 = 15.885000;
                    a4 = 2.851370; b4 = 137.903000; c = 1.984860;
        } else if (element.number() == 61) {
            a1 = 23.340500; b1 = 2.562700; a2 = 19.609501; b2 = 0.202088; a3 = 13.123500; b3 = 15.100900;
                    a4 = 2.875160; b4 = 132.720993; c = 2.028760;
        } else if (element.number() == 62) {
            a1 = 24.004200; b1 = 2.472740; a2 = 19.425800; b2 = 0.196451; a3 = 13.439600; b3 = 14.399600;
                    a4 = 2.896040; b4 = 128.007004; c = 2.209630;
        } else if (element.number() == 63) {
            a1 = 24.627399; b1 = 2.387900; a2 = 19.088600; b2 = 0.194200; a3 = 13.760300; b3 = 13.754600;
                    a4 = 2.922700; b4 = 123.174004; c = 2.574500;
        } else if (element.number() == 64) {
            a1 = 25.070900; b1 = 2.253410; a2 = 19.079800; b2 = 0.181951; a3 = 13.851800; b3 = 12.933100;
                    a4 = 3.545450; b4 = 101.398003; c = 2.419600;
        } else if (element.number() == 65) {
            a1 = 25.897600; b1 = 2.242560; a2 = 18.218500; b2 = 0.196143; a3 = 14.316700; b3 = 12.664800;
                    a4 = 2.953540; b4 = 115.362000; c = 3.589240;
        } else if (element.number() == 66) {
            a1 = 26.507000; b1 = 2.180200; a2 = 17.638300; b2 = 0.202172; a3 = 14.559600; b3 = 12.189900;
                    a4 = 2.965770; b4 = 111.874001; c = 4.297280;
        } else if (element.number() == 67) {
            a1 = 26.904900; b1 = 2.070510; a2 = 17.294001; b2 = 0.197940; a3 = 14.558300; b3 = 11.440700;
                    a4 = 3.638370; b4 = 92.656601; c = 4.567960;
        } else if (element.number() == 68) {
            a1 = 27.656300; b1 = 2.073560; a2 = 16.428499; b2 = 0.223545; a3 = 14.977900; b3 = 11.360400;
                    a4 = 2.982330; b4 = 105.703003; c = 5.920460;
        } else if (element.number() == 69) {
            a1 = 28.181900; b1 = 2.028590; a2 = 15.885100; b2 = 0.238849; a3 = 15.154200; b3 = 10.997500;
                    a4 = 2.987060; b4 = 102.960999; c = 6.756210;
        } else if (element.number() == 70) {
            a1 = 28.664101; b1 = 1.988900; a2 = 15.434500; b2 = 0.257119; a3 = 15.308700; b3 = 10.664700;
                    a4 = 2.989630; b4 = 100.417000; c = 7.566720;
        } else if (element.number() == 71) {
            a1 = 28.947599; b1 = 1.901820; a2 = 15.220800; b2 = 9.985190; a3 = 15.100000; b3 = 0.261033;
                    a4 = 3.716010; b4 = 84.329803; c = 7.976280;
        } else if (element.number() == 72) {
            a1 = 29.143999; b1 = 1.832620; a2 = 15.172600; b2 = 9.599900; a3 = 14.758600; b3 = 0.275116;
                    a4 = 4.300130; b4 = 72.028999; c = 8.581540;
        } else if (element.number() == 73) {
            a1 = 29.202400; b1 = 1.773330; a2 = 15.229300; b2 = 9.370460; a3 = 14.513500; b3 = 0.295977;
                    a4 = 4.764920; b4 = 63.364399; c = 9.243540;
        } else if (element.number() == 74) {
            a1 = 29.081800; b1 = 1.720290; a2 = 15.430000; b2 = 9.225900; a3 = 14.432700; b3 = 0.321703;
                    a4 = 5.119820; b4 = 57.056000; c = 9.887500;
        } else if (element.number() == 75) {
            a1 = 28.762100; b1 = 1.671910; a2 = 15.718900; b2 = 9.092270; a3 = 14.556400; b3 = 0.350500;
                    a4 = 5.441740; b4 = 52.086102; c = 10.472000;
        } else if (element.number() == 76) {
            a1 = 28.189400; b1 = 1.629030; a2 = 16.155001; b2 = 8.979480; a3 = 14.930500; b3 = 0.382661;
                    a4 = 5.675890; b4 = 48.164700; c = 11.000500;
        } else if (element.number() == 77) {
            a1 = 27.304899; b1 = 1.592790; a2 = 16.729601; b2 = 8.865530; a3 = 15.611500; b3 = 0.417916;
                    a4 = 5.833770; b4 = 45.001099; c = 11.472200;
        } else if (element.number() == 78) {
            a1 = 27.005899; b1 = 1.512930; a2 = 17.763901; b2 = 8.811740; a3 = 15.713100; b3 = 0.424593;
                    a4 = 5.783700; b4 = 38.610298; c = 11.688300;
        } else if (element.number() == 79) {
            a1 = 16.881901; b1 = 0.461100; a2 = 18.591299; b2 = 8.621600; a3 = 25.558201; b3 = 1.482600;
                    a4 = 5.860000; b4 = 36.395599; c = 12.065800;
        } else if (element.number() == 80) {
            a1 = 20.680901; b1 = 0.545000; a2 = 19.041700; b2 = 8.448400; a3 = 21.657499; b3 = 1.572900;
                    a4 = 5.967600; b4 = 38.324600; c = 12.608900;
        } else if (element.number() == 81) {
            a1 = 27.544600; b1 = 0.655150; a2 = 19.158400; b2 = 8.707510; a3 = 15.538000; b3 = 1.963470;
                    a4 = 5.525930; b4 = 45.814899; c = 13.174600;
        } else if (element.number() == 82) {
            a1 = 31.061701; b1 = 0.690200; a2 = 13.063700; b2 = 2.357600; a3 = 18.441999; b3 = 8.618000;
                    a4 = 5.969600; b4 = 47.257900; c = 13.411800;
        } else if (element.number() == 83) {
            a1 = 33.368900; b1 = 0.704000; a2 = 12.951000; b2 = 2.923800; a3 = 16.587700; b3 = 8.793700;
                    a4 = 6.469200; b4 = 48.009300; c = 13.578200;
        } else if (element.number() == 84) {
            a1 = 34.672600; b1 = 0.700999; a2 = 15.473300; b2 = 3.550780; a3 = 13.113800; b3 = 9.556420;
                    a4 = 7.025800; b4 = 47.004501; c = 13.677000;
        } else if (element.number() == 85) {
            a1 = 35.316299; b1 = 0.685870; a2 = 19.021099; b2 = 3.974580; a3 = 9.498870; b3 = 11.382400;
                    a4 = 7.425180; b4 = 45.471500; c = 13.710800;
        } else if (element.number() == 86) {
            a1 = 35.563099; b1 = 0.663100; a2 = 21.281601; b2 = 4.069100; a3 = 8.003700; b3 = 14.042200;
                    a4 = 7.443300; b4 = 44.247299; c = 13.690500;
        } else if (element.number() == 87) {
            a1 = 35.929901; b1 = 0.646453; a2 = 23.054701; b2 = 4.176190; a3 = 12.143900; b3 = 23.105200;
                    a4 = 2.112530; b4 = 150.645004; c = 13.724700;
        } else if (element.number() == 88) {
            a1 = 35.763000; b1 = 0.616341; a2 = 22.906401; b2 = 3.871350; a3 = 12.473900; b3 = 19.988701;
                    a4 = 3.210970; b4 = 142.324997; c = 13.621100;
        } else if (element.number() == 89) {
            a1 = 35.659698; b1 = 0.589092; a2 = 23.103201; b2 = 3.651550; a3 = 12.597700; b3 = 18.599001;
                    a4 = 4.086550; b4 = 117.019997; c = 13.526600;
        } else if (element.number() == 90) {
            a1 = 35.564499; b1 = 0.563359; a2 = 23.421900; b2 = 3.462040; a3 = 12.747300; b3 = 17.830900;
                    a4 = 4.807030; b4 = 99.172203; c = 13.431400;
        } else if (element.number() == 91) {
            a1 = 35.884701; b1 = 0.547751; a2 = 23.294800; b2 = 3.415190; a3 = 14.189100; b3 = 16.923500;
                    a4 = 4.172870; b4 = 105.250999; c = 13.428700;
        } else if (element.number() == 92) {
            a1 = 36.022800; b1 = 0.529300; a2 = 23.412800; b2 = 3.325300; a3 = 14.949100; b3 = 16.092699;
                    a4 = 4.188000; b4 = 100.612999; c = 13.396600;
        } else if (element.number() == 93) {
            a1 = 36.187401; b1 = 0.511929; a2 = 23.596399; b2 = 3.253960; a3 = 15.640200; b3 = 15.362200;
                    a4 = 4.185500; b4 = 97.490799; c = 13.357300;
        } else if (element.number() == 94) {
            a1 = 36.525398; b1 = 0.499384; a2 = 23.808300; b2 = 3.263710; a3 = 16.770700; b3 = 14.945500;
                    a4 = 3.479470; b4 = 105.980003; c = 13.381200;
        } else if (element.number() == 95) {
            a1 = 36.670601; b1 = 0.483629; a2 = 24.099199; b2 = 3.206470; a3 = 17.341499; b3 = 14.313600;
                    a4 = 3.493310; b4 = 102.273003; c = 13.359200;
        } else if (element.number() == 96) {
            a1 = 36.648800; b1 = 0.465154; a2 = 24.409599; b2 = 3.089970; a3 = 17.399000; b3 = 13.434600;
                    a4 = 4.216650; b4 = 88.483398; c = 13.288700;
        } else if (element.number() == 97) {
            a1 = 36.788101; b1 = 0.451018; a2 = 24.773600; b2 = 3.046190; a3 = 17.891899; b3 = 12.894600;
                    a4 = 4.232840; b4 = 86.002998; c = 13.275400;
        } else if (element.number() == 98) {
            a1 = 36.918499; b1 = 0.437533; a2 = 25.199499; b2 = 3.007750; a3 = 18.331699; b3 = 12.404400;
                    a4 = 4.243910; b4 = 83.788101; c = 13.267400;
        } else {

            Output::newline(ERROR);
                    Output::print("Atomic scattering factor is not defined for ");
                    Output::print(element.symbol());
                    Output::quit();
        }
        _atfParams[i].length(9);
                _atfParams[i][0] = a1;
                _atfParams[i][1] = a2;
                _atfParams[i][2] = a3;
                _atfParams[i][3] = a4;
                _atfParams[i][4] = b1;
                _atfParams[i][5] = b2;
                _atfParams[i][6] = b3;
                _atfParams[i][7] = b4;
                _atfParams[i][8] = c;
    }
}

/**
 * Calculate atomic scattering factor for a group of symmetrically-identical atoms
 * @param atfParams [in] Atomic form factor parameters
 * @param angle [in] Angle radiation is scattered into
 * @param wavelength [in] Wavelength of incident radiation
 * @return Atomic scattering factor for those atoms
 */
double CalculatedPeak::atomicScatteringFactor(List<double>& atfParams, double angle, double wavelength) {
    // Get s value
    double s = sin(angle) / wavelength;
	double s2 = s * s;
    if (s > 2) {
        Output::newline(WARNING);
                Output::print("Atomic scattering factor is not optimized for s greater than 2");
    }
            

    // Return result
    return atfParams[0] * exp(-atfParams[4] * s2) + \
           atfParams[1] * exp(-atfParams[5] * s2) + \
           atfParams[2] * exp(-atfParams[6] * s2) + \
           atfParams[3] * exp(-atfParams[7] * s2) + atfParams[8];
}

/**
 * Prints out a diffraction pattern so that it can be visualized in another program.
 * 
 * Note: twoTheta, Intensity, and fittedIntensity must all be the same length.
 * 
 * @param filename [in] Name of file in which to save raw pattern
 * @param twoTheta [in] Angles at which intensity is measured
 * @param Intensity [in] Intensity measured at each angle
 * @param fittedIntensity [in] Optional: Some other function that should be expressed as a function of angle
 */
void Diffraction::savePattern(const Word& filename, const vector<double>& twoTheta, \
        const vector<double>& Intensity, const vector<double>& otherIntensity) {
    int origStream = Output::streamID();
            Output::setStream(Output::addStream(filename));
            Output::newline();
    for (int i = 0; i < twoTheta.size(); i++) {
        Output::printPadded(twoTheta[i], 10, RIGHT, 3);
                Output::printPaddedSci(Intensity[i], 15, RIGHT, 5);

        if (otherIntensity.size() != 0)
                Output::printPaddedSci(otherIntensity[i], 15, RIGHT, 5);
                Output::newline();
        }
    Output::removeStream(Output::streamID());
            Output::setStream(origStream);
}

/**
 * Calculate the first derivative of a curve. x and y must be the same length. 
 * 
 * <p>For now, assumes that the spacing of points along x is uniform.
 * @param x Array of values of independent variable
 * @param y f(x)
 * @return f'(x) for each point. Same length as x and y
 */
vector<double> ExperimentalPattern::getFirstDerivative(const vector<double>& x, const vector<double>& y) {
    // Initialize output
    vector<double> d(x.size(), 0.0);
            // Calculate derivatives
            double h = 2 * (x[1] - x[0]);
            d[0] = (d[1] - d[0]) / h * 2;
    for (int i = 1; i < x.size() - 1; i++)
            d[i] = (y[i + 1] - y[i - 1]) / h;
            d.back() = (d.back() - d[d.size() - 2]) / h * 2;

    return d;
}

/**
 * Calculate the second derivative of a curve. x and y must be the same length. 
 * <p>For now, assumes that the spacing of points along x is uniform.
 * @param x Array of values of independent variable
 * @param y f(x)
 * @return f''(x) for each point. Same length as x and y
 */
vector<double> ExperimentalPattern::getSecondDerivative(const vector<double>& x, const vector<double>& y) {
    // Initialize output
    vector<double> d(x.size(), 0.0);
            // Calculate derivatives
            double h2 = (x[1] - x[0]); h2 *= h2;
    for (int i = 1; i < x.size() - 1; i++)
            d[i] = (y[i + 1] - 2 * y[i] + y[i - 1]) / h2;
            // Just store the 2nd and 2nd to last values as the first and last, respectively
            d[0] = d[1];
            d.back() = d[d.size() - 2];
    return d;
}
