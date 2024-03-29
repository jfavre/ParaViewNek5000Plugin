#include "vtkAutoInit.h" 
VTK_MODULE_INIT(vtkRenderingOpenGL2); // VTK was built with vtkRenderingOpenGL2
VTK_MODULE_INIT(vtkInteractionStyle);

#include "vtkCompositeDataPipeline.h"
#include "vtkGeometryFilter.h"
#include "vtkInformation.h"
#include "vtkLookupTable.h"
#include "vtkNek5000Reader.h"
#include "vtkNew.h"
#include "vtkPointData.h"
#include "vtkPolyDataMapper.h"
#include "vtkRenderer.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkTestUtilities.h"
#include "vtkUnstructuredGrid.h"

#include <vtksys/SystemTools.hxx>
#include <vtksys/CommandLineArguments.hxx>

#define WITH_GRAPHICS 1
int
vtkIONek5000CxxTests(int argc, char **argv)
{
  std::string filein;
  std::string varname;
  bool AnimateAlltimeSteps = false;
  double TimeStep = 0.0;
  int k, BlockIndex = 0;

  vtksys::CommandLineArguments args;
  args.Initialize(argc, argv);
  args.AddArgument(
    "-f", vtksys::CommandLineArguments::SPACE_ARGUMENT, &filein, "(the name of the Nek5000 file to read)");
  args.AddArgument(
    "-var", vtksys::CommandLineArguments::SPACE_ARGUMENT, &varname, "(the name of the SCALAR variable to display)");
  args.AddArgument(
    "-step", vtksys::CommandLineArguments::SPACE_ARGUMENT, &TimeStep, "(show a particular time step)");
  args.AddArgument(
    "-animate", vtksys::CommandLineArguments::NO_ARGUMENT, &AnimateAlltimeSteps, "(animate all steps)");

  if ( !args.Parse() || argc == 1 || filein.empty())
    {
    cerr << "\nTestReaderSimple: Written by Jean M. Favre\n"
         << "options are:\n";
    cerr << args.GetHelp() << "\n";
    return EXIT_FAILURE;
    }

  if(!vtksys::SystemTools::FileExists(filein.c_str()))
    {
    cerr << "\nFile " << filein.c_str() << " does not exist\n\n";
    return EXIT_FAILURE;
    }

  vtkNew<vtkNek5000Reader> reader;
  reader->DebugOff();
  reader->SetFileName(filein.c_str());
  reader->UpdateInformation();
  reader->DisableAllPointArrays();
  reader->SetPointArrayStatus(varname.c_str(), 1);
 
  reader->UpdateTimeStep(TimeStep); // time value
  reader->Update();

  double range[2];
  if(varname.size())
    {
    reader->GetOutput()->GetPointData()->GetScalars(varname.c_str())->GetRange(range);
    cerr << varname.c_str() << ": scalar range = [" << range[0] << ", " << range[1] << "]\n";
    }

#ifdef WITH_GRAPHICS

  vtkNew<vtkLookupTable> lut;
  lut->SetHueRange(0.66,0.0);
  lut->SetNumberOfTableValues(256);
  lut->Build();

  if(varname.size())
    {
    lut->SetTableRange(range[0], range[1]);
    lut->Build();
    }

  vtkNew<vtkGeometryFilter> geom;
  geom->SetInputConnection(reader->GetOutputPort(0));

  vtkNew<vtkPolyDataMapper> mapper1;
  mapper1->SetInputConnection(geom->GetOutputPort(0));
  mapper1->ScalarVisibilityOn();
  mapper1->SetScalarModeToUsePointFieldData();

  if(varname.size())
    {
    mapper1->SelectColorArray(varname.c_str());
    mapper1->SetLookupTable(lut);
    mapper1->UseLookupTableScalarRangeOn();
    }
  vtkNew<vtkActor> actor1;
  actor1->SetMapper(mapper1);

  vtkNew<vtkRenderer> ren;
  vtkNew<vtkRenderWindow> renWin;
  vtkNew<vtkRenderWindowInteractor> iren;

  iren->SetRenderWindow(renWin);
  renWin->AddRenderer(ren);
  ren->AddActor(actor1);

  renWin->SetSize(512, 512);
  renWin->Render();
  ren->ResetCamera();

  renWin->Render();

  if(AnimateAlltimeSteps)
    {
    vtkInformation *execInfo = geom->GetExecutive()->GetOutputInformation(0);
    if (execInfo->Has(vtkStreamingDemandDrivenPipeline::TIME_STEPS())) 
	{
        int NumberOfTimeSteps = execInfo->Length(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
        double *TimeSteps = new double[NumberOfTimeSteps];
        execInfo->Get(vtkStreamingDemandDrivenPipeline::TIME_STEPS(), TimeSteps);
        for(int i=0; i < NumberOfTimeSteps; i++)
          { 
          execInfo->Set(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP(), TimeSteps[i]);
          renWin->Render();
          }
        delete [] TimeSteps;
	}
    }
  iren->Start();
#endif
  return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
  vtkIONek5000CxxTests(argc, argv);
}
