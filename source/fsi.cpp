#include "fsi.h"
#include <complex>
#include <iostream>

template <int dim>
FSI<dim>::FSI(Fluid::FluidSolver<dim> &f,
              Solid::SolidSolver<dim> &s,
              const Parameters::AllParameters &p)
  : fluid_solver(f),
    solid_solver(s),
    parameters(p),
    time(parameters.end_time,
         parameters.time_step,
         parameters.output_interval,
         parameters.refinement_interval,
         parameters.save_interval)
{
}

template <int dim>
void FSI<dim>::move_solid_mesh(bool move_forward)
{
  std::vector<bool> vertex_touched(solid_solver.triangulation.n_vertices(),
                                   false);
  for (auto cell = solid_solver.dof_handler.begin_active();
       cell != solid_solver.dof_handler.end();
       ++cell)
    {
      for (unsigned int v = 0; v < GeometryInfo<dim>::vertices_per_cell; ++v)
        {
          if (!vertex_touched[cell->vertex_index(v)])
            {
              vertex_touched[cell->vertex_index(v)] = true;
              Point<dim> vertex_displacement;
              for (unsigned int d = 0; d < dim; ++d)
                {
                  vertex_displacement[d] = solid_solver.current_displacement(
                    cell->vertex_dof_index(v, d));
                }
              if (move_forward)
                {
                  cell->vertex(v) += vertex_displacement;
                }
              else
                {
                  cell->vertex(v) -= vertex_displacement;
                }
            }
        }
    }
}

template <int dim>
bool FSI<dim>::point_in_mesh(const DoFHandler<dim> &df, const Point<dim> &point)
{
  for (auto cell = df.begin_active(); cell != df.end(); ++cell)
    {
      if (cell->point_inside(point))
        {
          return true;
        }
    }
  return false;
}

template <int dim>
void FSI<dim>::update_solid_displacement()
{
  move_solid_mesh(true);
  auto displacement = solid_solver.current_displacement;
  std::vector<bool> vertex_touched(solid_solver.dof_handler.n_dofs(), false);
  for (auto cell : solid_solver.dof_handler.active_cell_iterators())
    {
      for (unsigned int v = 0; v < GeometryInfo<dim>::vertices_per_cell; ++v)
        {
          if (!vertex_touched[cell->vertex_index(v)])
            {
              vertex_touched[cell->vertex_index(v)] = true;
              Point<dim> point = cell->vertex(v);
              Vector<double> tmp(dim+1);
              VectorTools::point_value(fluid_solver.dof_handler,
                                       fluid_solver.present_solution,
                                       point,
                                       tmp);
              for (unsigned int d = 0; d < dim; ++d)
                {
                  displacement[cell->vertex_dof_index(v, d)] +=
                    tmp[d] * time.get_delta_t();
                }
            }
        }
    }
  move_solid_mesh(false);
  solid_solver.current_displacement = displacement;
}

// Dirichlet bcs are applied to artificial fluid cells, so fluid nodes should
// be marked as artificial or real. Meanwhile, additional body force is
// applied to the artificial fluid quadrature points. To accomodate these two
// settings, we define indicator at quadrature points, but only when all
// of the vertices of a fluid cell are found to be in solid domain,
// set the indicators at all quadrature points to be 1.
template <int dim>
void FSI<dim>::update_indicator()
{
  move_solid_mesh(true);
  FEValues<dim> fe_values(fluid_solver.fe,
                          fluid_solver.volume_quad_formula,
                          update_quadrature_points);
  const unsigned int n_q_points = fluid_solver.volume_quad_formula.size();
  for (auto f_cell = fluid_solver.dof_handler.begin_active();
       f_cell != fluid_solver.dof_handler.end();
       ++f_cell)
    {
      fe_values.reinit(f_cell);
      auto p = fluid_solver.cell_property.get_data(f_cell);
      bool is_solid = true;
      for (unsigned int v = 0; v < GeometryInfo<dim>::vertices_per_cell; ++v)
        {
          Point<dim> point = f_cell->vertex(v);
          if (!point_in_mesh(solid_solver.dof_handler, point))
            {
              is_solid = false;
              break;
            }
        }
      for (unsigned int q = 0; q < n_q_points; ++q)
        {
          p[q]->indicator = is_solid;
        }
    }
  move_solid_mesh(false);
}

// This function interpolates the solid velocity into the fluid solver,
// as the Dirichlet boundary conditions for artificial fluid vertices
template <int dim>
void FSI<dim>::find_fluid_bc()
{
  move_solid_mesh(true);

  const unsigned int n_q_points = fluid_solver.volume_quad_formula.size();
  FEValues<dim> fe_values(fluid_solver.fe,
                          fluid_solver.volume_quad_formula,
                          update_values | update_quadrature_points |
                            update_JxW_values | update_gradients);
  const FEValuesExtractors::Vector velocities(0);
  const FEValuesExtractors::Scalar pressure(dim);
  std::vector<SymmetricTensor<2, dim>> sym_grad_v(n_q_points);
  std::vector<double> p(n_q_points);
  std::vector<Tensor<2, dim>> grad_v(n_q_points);
  std::vector<Tensor<1, dim>> v(n_q_points);
  std::vector<Tensor<1, dim>> dv(n_q_points);

  for (auto f_cell = fluid_solver.dof_handler.begin_active();
       f_cell != fluid_solver.dof_handler.end();
       ++f_cell)
    {
      auto ptr = fluid_solver.cell_property.get_data(f_cell);
      fe_values.reinit(f_cell);
      // Fluid velocity increment
      fe_values[velocities].get_function_values(fluid_solver.solution_increment,
                                                dv);
      // Fluid velocity gradient
      fe_values[velocities].get_function_gradients(
        fluid_solver.present_solution, grad_v);
      // Fluid symmetric velocity gradient
      fe_values[velocities].get_function_symmetric_gradients(
        fluid_solver.present_solution, sym_grad_v);
      // Fluid pressure
      fe_values[pressure].get_function_values(fluid_solver.present_solution, p);
      // Loop over all quadrature points to set FSI forces.
      for (unsigned int q = 0; q < n_q_points; ++q)
        {
          Point<dim> point = fe_values.quadrature_point(q);
          ptr[q]->indicator = point_in_mesh(solid_solver.dof_handler, point);
          ptr[q]->fsi_acceleration = 0;
          ptr[q]->fsi_stress = 0;
          if (ptr[q]->indicator == 0)
            continue;
          // acceleration: Dv^f/Dt - Dv^s/Dt
          Tensor<1, dim> fluid_acc =
            dv[q] / time.get_delta_t() + grad_v[q] * v[q];
          (void)fluid_acc;
          Vector<double> solid_acc(dim);
          VectorTools::point_value(solid_solver.dof_handler,
                                   solid_solver.current_acceleration,
                                   point,
                                   solid_acc);
          for (unsigned int i = 0; i < dim; ++i)
            {
              ptr[q]->fsi_acceleration[i] =
                parameters.gravity[i] - solid_acc[i];
            }
          // stress: sigma^f - sigma^s
          SymmetricTensor<2, dim> solid_sigma;
          for (unsigned int i = 0; i < dim; ++i)
            {
              for (unsigned int j = 0; j < dim; ++j)
                {
                  Vector<double> sigma_ij(1);
                  VectorTools::point_value(solid_solver.scalar_dof_handler,
                                           solid_solver.stress[i][j],
                                           point,
                                           sigma_ij);
                  solid_sigma[i][j] = sigma_ij[0];
                }
            }
          ptr[q]->fsi_stress =
            -p[q] * Physics::Elasticity::StandardTensors<dim>::I +
            parameters.viscosity * sym_grad_v[q] - solid_sigma;
        }
    }
  move_solid_mesh(false);
}

template <int dim>
void FSI<dim>::find_solid_bc()
{
  // Must use the updated solid coordinates
  move_solid_mesh(true);
  // Fluid FEValues to do interpolation
  FEValues<dim> fe_values(
    fluid_solver.fe, fluid_solver.volume_quad_formula, update_values);
  // Solid FEFaceValues to get the normal
  FEFaceValues<dim> fe_face_values(solid_solver.fe,
                                   solid_solver.face_quad_formula,
                                   update_quadrature_points |
                                     update_normal_vectors);

  const unsigned int n_face_q_points = solid_solver.face_quad_formula.size();

  for (auto s_cell = solid_solver.dof_handler.begin_active();
       s_cell != solid_solver.dof_handler.end();
       ++s_cell)
    {
      auto ptr = solid_solver.cell_property.get_data(s_cell);
      for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f)
        {
          // Current face is at boundary and without Dirichlet bc.
          if (s_cell->face(f)->at_boundary())
            {
              fe_face_values.reinit(s_cell, f);
              for (unsigned int q = 0; q < n_face_q_points; ++q)
                {
                  Point<dim> q_point = fe_face_values.quadrature_point(q);
                  Tensor<1, dim> normal = fe_face_values.normal_vector(q);
                  Vector<double> value(dim + 1);
                  Utils::GridInterpolator<dim, BlockVector<double>>
                    interpolator(fluid_solver.dof_handler, q_point);
                  interpolator.point_value(fluid_solver.present_solution,
                                           value);
                  std::vector<Tensor<1, dim>> gradient(dim + 1,
                                                       Tensor<1, dim>());
                  interpolator.point_gradient(fluid_solver.present_solution,
                                              gradient);
                  SymmetricTensor<2, dim> sym_deformation;
                  for (unsigned int i = 0; i < dim; ++i)
                    {
                      for (unsigned int j = 0; j < dim; ++j)
                        {
                          sym_deformation[i][j] =
                            (gradient[i][j] + gradient[j][i]) / 2;
                        }
                    }
                  // \f$ \sigma = -p\bold{I} + \mu\nabla^S v\f$
                  SymmetricTensor<2, dim> stress =
                    -value[dim] * Physics::Elasticity::StandardTensors<dim>::I +
                    parameters.viscosity * sym_deformation;
                  ptr[f * n_face_q_points + q]->fsi_traction = stress * normal;
                }
            }
        }
    }
  move_solid_mesh(false);
}

template <int dim>
void FSI<dim>::refine_mesh(const unsigned int min_grid_level,
                           const unsigned int max_grid_level)
{
  move_solid_mesh(true);
  for (auto f_cell : fluid_solver.dof_handler.active_cell_iterators())
    {
      auto center = f_cell->center();
      double dist = 1000;
      for (auto s_cell : solid_solver.dof_handler.active_cell_iterators())
        {
          dist = std::min(center.distance(s_cell->center()), dist);
        }
      if (dist < 0.1)
        f_cell->set_refine_flag();
      else
        f_cell->set_coarsen_flag();
    }
  move_solid_mesh(false);
  if (fluid_solver.triangulation.n_levels() > max_grid_level)
    {
      for (auto cell = fluid_solver.triangulation.begin_active(max_grid_level);
           cell != fluid_solver.triangulation.end();
           ++cell)
        {
          cell->clear_refine_flag();
        }
    }

  for (auto cell = fluid_solver.triangulation.begin_active(min_grid_level);
       cell != fluid_solver.triangulation.end_active(min_grid_level);
       ++cell)
    {
      cell->clear_coarsen_flag();
    }

  BlockVector<double> buffer(fluid_solver.present_solution);
  SolutionTransfer<dim, BlockVector<double>> solution_transfer(
    fluid_solver.dof_handler);

  fluid_solver.triangulation.prepare_coarsening_and_refinement();
  solution_transfer.prepare_for_coarsening_and_refinement(buffer);

  fluid_solver.triangulation.execute_coarsening_and_refinement();

  fluid_solver.setup_dofs();
  fluid_solver.make_constraints();
  fluid_solver.initialize_system();

  solution_transfer.interpolate(buffer, fluid_solver.present_solution);
  fluid_solver.nonzero_constraints.distribute(fluid_solver.present_solution);
}

template <int dim>
void FSI<dim>::run()
{
  solid_solver.triangulation.refine_global(parameters.global_refinements[1]);
  solid_solver.setup_dofs();
  solid_solver.initialize_system();
  fluid_solver.triangulation.refine_global(parameters.global_refinements[0]);
  fluid_solver.setup_dofs();
  fluid_solver.make_constraints();
  fluid_solver.initialize_system();

  std::cout << "Number of fluid active cells and dofs: ["
            << fluid_solver.triangulation.n_active_cells() << ", "
            << fluid_solver.dof_handler.n_dofs() << "]" << std::endl
            << "Number of solid active cells and dofs: ["
            << solid_solver.triangulation.n_active_cells() << ", "
            << solid_solver.dof_handler.n_dofs() << "]" << std::endl;

  bool first_step = true;
  while (time.end() - time.current() > 1e-12)
    {
      find_solid_bc();
      solid_solver.run_one_step(first_step);
      find_fluid_bc();
      fluid_solver.run_one_step(first_step);
      first_step = false;
      time.increment();
      if (time.time_to_refine())
        refine_mesh(parameters.global_refinements[0],
                    parameters.global_refinements[0] + 2);
    }
}

template class FSI<2>;
template class FSI<3>;
