// DEBUG
#include <iostream>
#include <vector>
#include "FluidSolver.h"
#include "IFluidRenderer.h"
#include "Grid.h"
#include "Cell.h"
#include "Vector2.h"
#include "SignalRelay.h"


using std::vector;

FluidSolver::FluidSolver(float width, float height)
  : _width(width),
    _height(height),
    _grid(_width, _height),
    _frameReady(false),
    _particles()
{
  // Provide default values to the grid.
  reset();

  // Connect ourselves to the 'resetSimulation' signal.
  QObject::connect(SignalRelay::getInstance(), SIGNAL(resetSimulation()),
		   this, SLOT(reset()));
}


FluidSolver::~FluidSolver()
{
  // Disconnect from receiving any signals.
  SignalRelay::getInstance()->disconnect(this);
}


void FluidSolver::reset()
{
  // Note: This is super ghetto currently, and simply initializes the solver
  // with a large default grid, all filled with fluid and arbitrary velocities.
  // Change this as needed during devlopment to quickly test stuff out, and
  // remove before final release!

  // Delete existing particles
  _particles.clear();
  
  // Initialize a velocity field for testing.
  // Note: values in this velocity field are arbitrarily chosen and may be
  //  divergent within a cell.
  // Note: sin() is used to clamp output values to [-1, 1].
  Grid grid(_width, _height);
  for (float y = 0; y < _height; ++y) 
    for (float x = 0; x < _width; ++x) {
      grid(x,y).cellType = Cell::FLUID;
      grid(x,y).pressure = 1.0f;
      grid(x,y).vel[Cell::X] = sin(x * 45.215 + y * 88.15468) / 2; // arbitrary constants
      grid(x,y).vel[Cell::Y] = sin(x * 2.548 + y * 121.1215) / 2;  // arbitrary constants

      // Initialize particle positions
      for (unsigned i = 0; i < 4; i++)
        for(unsigned j = 0; j < 4; j++) 
        _particles.push_back(Vector2(x + 0.20f * (i + 1), y + 0.20f * (j + 1)));
  }

  // Set values accordingly.
  _grid = grid;
  _frameReady = false;
}

void FluidSolver::advanceFrame()
{
  float frameTimeSec = 1.0f/30.0f; // TODO Target 30 Hz framerate for now.
  float CFLCoefficient = 2.0f;     // TODO CFL coefficient set to 2 for now.

  while (!_frameReady) {
    // If enough simulation time has elapsed to draw the next frame, break.
    if (frameTimeSec <= 0.0f) {
      _frameReady = true;
      break;
    }

    // Calculate an appropriate timestep based on the estimated max velocity
    // and the CFL coefficient.
    float simTimeStepSec = CFLCoefficient / _grid.getMaxVelocity().magnitude();
    
    // If the remaining time to simulate in this frame is less than the
    // CFL-calculated time, just advance the sim for the remaining frame time.
    if (simTimeStepSec > frameTimeSec) {
      simTimeStepSec = frameTimeSec;
    }
    
    // Advance the simulation by the calculated timestep.  Decrement the
    // "remaining" time for this frame.
    advanceTimeStep(simTimeStepSec);
    frameTimeSec -= simTimeStepSec;
  }
}


void FluidSolver::advanceTimeStep(float timeStepSec)
{
  Vector2 gravity(0.0f, -0.098);  // Gravity: -0.098 cells/sec^2

  advectVelocity(timeStepSec);
  applyGlobalVelocity(gravity * timeStepSec);
  pressureSolve(timeStepSec);
  boundaryCollide();
  moveParticles(timeStepSec);
}


void FluidSolver::advectVelocity(float timeStepSec)
{
  for(float x = 0; x < _grid.getWidth(); x += 1.0f)
    for( float y = 0.5; y < _grid.getHeight(); y += 1.0f) {
      Cell &cell = _grid(floor(x), floor(y));
      Vector2 position(x, y); 
      position += _grid.getVelocity(position) * -timeStepSec;
      if (position.x < 0)
        position.zeroX();
      if (position.y > _grid.getWidth())
        position.x = _grid.getWidth(); 
      if (position.y < 0)
        position.zeroY();
      if (position.y > _grid.getHeight())
        position.y = _grid.getHeight();
      cell.stagedVel[Cell::X] = _grid.getVelocity(position).x;
    }
  for(float y = 0; y < _grid.getHeight(); y += 1.0f)
    for(float x = 0.5; x < _grid.getWidth(); x+= 1.0f) {
      Cell &cell = _grid(floor(x), floor(y));
      Vector2 position(x, y); 
      position += _grid.getVelocity(position) * -timeStepSec;
      if (position.x < 0)
        position.zeroX();
      if (position.y > _grid.getWidth())
        position.x = _grid.getWidth();
      if (position.y < 0)
        position.zeroY();
      if (position.y > _grid.getHeight())
        position.y = _grid.getHeight();
      cell.stagedVel[Cell::Y] = _grid.getVelocity(position).y;
    }
  for(unsigned i = 0; i < _grid.getRowCount() * _grid.getColCount(); i++) {
    _grid[i].commitStagedVel();
  }
}


void FluidSolver::applyGlobalVelocity(Vector2 velocity)
{
  // Apply the provided velocity to all cells in the simulation.
  unsigned cellCount = _grid.getRowCount() * _grid.getColCount();
  for (unsigned i = 0; i < cellCount; ++i) {
    _grid[i].vel[Cell::X] += velocity.x;
    _grid[i].vel[Cell::Y] += velocity.y;
  }
}


void FluidSolver::pressureSolve(float timeStepSec)
{
  // NOTE: assumptions are made here that the only "SOLID" cells in the sim
  // are the walls of the simulation, and are therefore not handled in this
  // method.  Instead, they are handled in the boundaryCollide method where
  // velocities entering or exiting a boundary are simply set to 0.0f.

  // Modify velocity field based on pressure scalar field.
  for (unsigned x = 0; x < _grid.getColCount(); ++x)
    for (unsigned y = 0; y < _grid.getRowCount(); ++y) {
      Cell &cell = _grid(x,y);
      float pressureVel = timeStepSec * cell.pressure;
      if (cell.cellType == Cell::FLUID) {
	// Update all neighboring velocities touched by this pressure.
	cell.vel[Cell::X] -= pressureVel;
	cell.vel[Cell::Y] -= pressureVel;
	if (cell.neighbors[Cell::POS_X])
	  cell.neighbors[Cell::POS_X]->vel[Cell::X] += pressureVel;
	if (cell.neighbors[Cell::POS_Y])
	  cell.neighbors[Cell::POS_Y]->vel[Cell::Y] += pressureVel;
      }
    }
  
  // Calculate divergence throughout the simulation.
  // TODO hackish.
  vector<float> divergence(_grid.getColCount() * _grid.getRowCount(), 0.0f);
  for (unsigned x = 0; x < _grid.getColCount(); ++x)
    for (unsigned y = 0; y < _grid.getRowCount(); ++y) {
      unsigned index = y * _grid.getColCount() + x;
      divergence[index] = - _grid.getVelocityDivergence(x, y);
    }

  // Solve for new pressure values.
  // TODO.

}


void FluidSolver::boundaryCollide()
{
  // Set all boundary velocities to zero in the MAC grid.
  // This prevents fluid from entering or exiting through the walls of the
  // simulation.
  const unsigned rows = _grid.getRowCount();
  const unsigned cols = _grid.getColCount();

  // Bottom row. Set velocity Y component to 0.
  // Top row. Set velocity X and Y components to 0. Set to SOLID.
  for (unsigned col = 0; col < cols; ++col) {
    _grid(col, 0).vel[Cell::Y] = 0.0f;
    _grid(col, rows-1).vel[Cell::X] = 0.0f;
    _grid(col, rows-1).vel[Cell::Y] = 0.0f;
    _grid(col, rows-1).cellType = Cell::SOLID;
  }

  // Left column. Set velocity X component to 0.
  // Right column. Set velocity X and Y components to 0. Set to SOLID.
  for (unsigned row = 0; row < rows; ++row) {
    _grid(0, row).vel[Cell::X] = 0.0f;
    _grid(cols-1, row).vel[Cell::X] = 0.0f;
    _grid(cols-1, row).vel[Cell::Y] = 0.0f;
    _grid(cols-1, row).cellType = Cell::SOLID;
  }
}


void FluidSolver::moveParticles(float timeStepSec)
{
  for (vector<Vector2>::iterator itr = _particles.begin();
                                 itr != _particles.end();
                                 ++itr)
  {
    *itr += _grid.getVelocity(*itr) * timeStepSec;
  }
}


void FluidSolver::draw(IFluidRenderer *renderer)
{
  // If a new frame is ready for rendering, draw it.
  if (_frameReady) {
    renderer->drawGrid(_grid, _particles);
    _frameReady = false;
  }
  else {
    // TODO, redraw previous frame.
  }
}


float FluidSolver::getSimulationWidth() const
{
  return _width;
}
  

float FluidSolver::getSimulationHeight() const
{
  return _height;
}
