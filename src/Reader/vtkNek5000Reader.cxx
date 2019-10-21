#include "vtkNek5000Reader.h"

#include "vtkObjectFactory.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkUnstructuredGrid.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkTrivialProducer.h"
#include "vtkCleanUnstructuredGrid.h"
#include "vtkDataArraySelection.h"

#include "vtkFloatArray.h"
#include "vtkUnsignedCharArray.h"
#include "vtkTypeUInt32Array.h"
#include "vtkCellType.h"
#include "vtkPointData.h"
#include "vtkCellData.h"
#include "vtkMultiProcessController.h"
#include "vtkNew.h"
#include "vtkSmartPointer.h"
#include "vtkTimerLog.h"
#include <vtksys/SystemTools.hxx>
#include <map>
#include <new>
#include <string>

vtkStandardNewMacro(vtkNek5000Reader);

void ByteSwap32(void *aVals, int nVals);
void ByteSwap64(void *aVals, int nVals);
int compare_ids(const void *id1, const void *id2);

//----------------------------------------------------------------------------

vtkNek5000Reader::vtkNek5000Reader(){
  this->DebugOff();
  //vtkDebugMacro(<<"vtkNek5000Reader::vtkNek5000Reader(): ENTER");

  // by default assume filters have one input and one output
  // subclasses that deviate should modify this setting
  this->SetNumberOfInputPorts(0);
  this->SetNumberOfOutputPorts(1);

  this->FileName = nullptr;
  this->DataFileName = nullptr;

  this->UGrid = nullptr;

  this->READ_GEOM_FLAG = true;
  this->CALC_GEOM_FLAG = true;
  this->IAM_INITIALLIZED = false;
  this->I_HAVE_DATA = false;
  this->FIRST_DATA = true;
  this->MeshIs3D = true;
  this->swapEndian = false;
  this->ActualTimeStep = 0;
  this->TimeStepRange[0] = 0;
  this->TimeStepRange[1] = 0;
  this->NumberOfTimeSteps = 0;
  this->displayed_step = -1;
  this->memory_step = -1;
  this->requested_step = -1;

  this->num_vars = 0;
  this->var_names = nullptr;
  this->var_length = nullptr;
  this->dataArray = nullptr;
  this->meshCoords = nullptr;
  this->myBlockIDs = nullptr;
  this->myBlockPositions = nullptr;
  this->use_variable = nullptr;
  this->timestep_has_mesh = nullptr;
  this->proc_numBlocks = nullptr;
  this->velocity_index = -1;

  this->PointDataArraySelection = vtkDataArraySelection::New();

  this->myList = new nek5KList();
}

//----------------------------------------------------------------------------
vtkNek5000Reader::~vtkNek5000Reader()
{
  if(this->use_variable)
    delete [] this->use_variable;
  if(this->timestep_has_mesh)
    delete [] this->timestep_has_mesh;
  if(this->FileName)
    delete [] this->FileName;
  if(this->DataFileName)
    delete [] this->DataFileName;

  if (this->myList)
  {
    delete this->myList;
  }
      
  if(this->dataArray)
  {
    vtkDebugMacro(<<"~vtkNek5000Reader():: Release memory for dataArrays");

    for(auto i=0; i<this->num_vars; i++)
    {
      if(this->dataArray[i])
      {
        std::cerr<< "freeing dataArray[" << this->var_names[i] << "]\n";
        delete [] this->dataArray[i];
      }
    }
    delete [] this->dataArray;
  }

  if(this->num_vars>0)
  {
    for (auto i=0; i<this->num_vars; i++)
      if(this->var_names[i])
      {
        free(this->var_names[i]);
      }
  }
  if(this->proc_numBlocks)
  {
    delete [] this->proc_numBlocks;
  }
  
  if(this->UGrid)
  {
    this->UGrid->Delete();
  }

  if(this->var_length)
    delete [] this->var_length;
  if(this->var_names)
    free(this->var_names);
  this->PointDataArraySelection->Delete();
  
  if(this->myBlockPositions)
    delete [] this->myBlockPositions;
}

//----------------------------------------------------------------------------


void vtkNek5000Reader::GetAllTimesAndVariableNames(vtkInformationVector *outputVector)
{
//  FILE* dfPtr = NULL;
  ifstream dfPtr;
  char dummy[64];
  double t;
  int    c;
  string v;

  char dfName[265];
  char* scan_ret;
  char* p;
  char* p2;
  char firstTags[32];
  char param[32];
  int file_index;
  float test_time_val;

  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  //vtkInformation* outInfo1 = outputVector->GetInformationObject(1);

  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    num_ranks = ctrl->GetNumberOfProcesses();
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    num_ranks = 1;
    my_rank = 0;
  }

  this->TimeStepRange[0] = 0;
  this->TimeStepRange[1] = this->NumberOfTimeSteps-1;

  this->TimeSteps.resize(this->NumberOfTimeSteps);
  this->timestep_has_mesh =  new bool[this->NumberOfTimeSteps];

  for (int i=0; i<(this->NumberOfTimeSteps); i++)
  {
      this->timestep_has_mesh[i] = false;    
      file_index = this->datafile_start + i;

      sprintf(dfName, this->datafile_format.c_str(), 0, file_index );
      vtkDebugMacro(<< "vtkNek5000Reader::GetAllTimesAndVariableNames:  this->datafile_start = "<< this->datafile_start<<"  i: " << i << " file_index: "<<file_index<< " dfName: " << dfName );

      dfPtr.open(dfName);

      if ( (dfPtr.rdstate() & std::ifstream::failbit ) != 0 )
          std::cerr << "Error opening : " << dfName << endl;

      dfPtr >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy;
      dfPtr >> t >> c >> dummy;
      vtkDebugMacro(<< "vtkNek5000Reader::GetAllTimesAndVariableNames:  time = "<< t <<" cycle =  " << c );
      //I do this to skip the num directories token, because it may abut
      //the field tags without a whitespace separator.
      while (dfPtr.peek() == ' ')
          dfPtr.get();
      while (dfPtr.peek() >= '0' && dfPtr.peek() <= '9')
          dfPtr.get();

      char tmpTags[32];
      dfPtr.read(tmpTags, 32);
      tmpTags[31] = '\0';

      v = tmpTags;

      // for the first time step on the master
      if(0==i)
      {
          // store the tags for the first step, and share with other procs to parse for variables
          strcpy(firstTags, tmpTags);
      }

      this->TimeSteps[i] = t;

      // If this file contains a mesh, the first variable codes after the
      // cycle number will be X Y
      if (v.find("X") != std::string::npos)
          this->timestep_has_mesh[i] = true;

      dfPtr.close();
    
      vtkDebugMacro(<<"vtkNek5000Reader::GetAllTimesAndVariableNames: this->TimeSteps["<<i<<"]= " <<this->TimeSteps[i]<<"  this->timestep_has_mesh["<<i<<"] = "<< this->timestep_has_mesh[i]);
    
  } // for (int i=0; i<(this->NumberOfTimeSteps); i++)

  this->GetVariableNamesFromData(firstTags);

  outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
               &(*this->TimeSteps.begin()),
               this->TimeSteps.size());

  double timeRange[2];
  timeRange[0] = *this->TimeSteps.begin();

  vtkDebugMacro(<< "vtkNek5000Reader::GetAllTimes: timeRange[0] = "<<timeRange[0]<< ", timeRange[1] = "<< timeRange[1]);

  outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(),
               timeRange, 2);

} // vtkNek5000Reader::GetAllTimes()

//----------------------------------------------------------------------------
unsigned long vtkNek5000Reader::GetMTime()
{
  unsigned long mTime = this->Superclass::GetMTime();
  unsigned long time;

  time = this->PointDataArraySelection->GetMTime();
  mTime = ( time > mTime ? time : mTime );

  return mTime;
}

//----------------------------------------------------------------------------
void vtkNek5000Reader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//----------------------------------------------------------------------------
int vtkNek5000Reader::GetNumberOfPointArrays()
{
  return this->PointDataArraySelection->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
const char* vtkNek5000Reader::GetPointArrayName(int index)
{
  return this->PointDataArraySelection->GetArrayName(index);
}

//----------------------------------------------------------------------------
int vtkNek5000Reader::GetPointArrayStatus(const char* name)
{
  return this->PointDataArraySelection->ArrayIsEnabled(name);
}

//----------------------------------------------------------------------------
int vtkNek5000Reader::GetPointArrayStatus(int index)
{
  return this->PointDataArraySelection->GetArraySetting(index);
}

//----------------------------------------------------------------------------
void vtkNek5000Reader::SetPointArrayStatus(const char* name, int status)
{
  if(status)
  {
    this->PointDataArraySelection->EnableArray(name);
  }
  else
  {
    this->PointDataArraySelection->DisableArray(name);
  }
}

//----------------------------------------------------------------------------
void vtkNek5000Reader::EnableAllPointArrays()
{
  this->PointDataArraySelection->EnableAllArrays();
}

//----------------------------------------------------------------------------
void vtkNek5000Reader::DisableAllPointArrays()
{
  this->PointDataArraySelection->DisableAllArrays();
}

#ifdef unused
//----------------------------------------------------------------------------
int vtkNek5000Reader::GetNumberOfDerivedVariableArrays()
{
  return this->DerivedVariableDataArraySelection->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
const char* vtkNek5000Reader::GetDerivedVariableArrayName(int index)
{
  return this->DerivedVariableDataArraySelection->GetArrayName(index);
}

//----------------------------------------------------------------------------
int vtkNek5000Reader::GetDerivedVariableArrayStatus(const char* name)
{
  return this->DerivedVariableDataArraySelection->ArrayIsEnabled(name);
}

//----------------------------------------------------------------------------
void vtkNek5000Reader::SetDerivedVariableArrayStatus(const char* name, int status)
{
  if(status)
  {
    this->DerivedVariableDataArraySelection->EnableArray(name);
  }
  else
  {
    this->DerivedVariableDataArraySelection->DisableArray(name);
  }
}

//----------------------------------------------------------------------------
void vtkNek5000Reader::EnableAllDerivedVariableArrays()
{
  this->DerivedVariableDataArraySelection->EnableAllArrays();
}

//----------------------------------------------------------------------------
void vtkNek5000Reader::DisableAllDerivedVariableArrays()
{
  this->DerivedVariableDataArraySelection->DisableAllArrays();
}
#endif
//----------------------------------------------------------------------------

void vtkNek5000Reader::updateVariableStatus()
{
  int my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    my_rank = 0;
  }
//vtkDebugMacro(<<"vtkNek5000Reader::updateVariableStatus: Rank: "<<my_rank<< " ENTER ");
  this->num_used_vectors=0;
  this->num_used_scalars=0;

// set all variables to false
  for(auto i=0; i<this->num_vars; i++)
  {
    this->use_variable[i] = false;
  }

  // if a variable is used, set it to true
  for(auto i=0; i<this->num_vars; i++)
  {
    if(this->GetPointArrayStatus(i) == 1)
    {
      this->use_variable[i] = true;

      // increment the number of vectors or scalars used, accordingly
      if(this->var_length[i] > 1)
      {
        this->num_used_vectors++;
      }
      else
      {
        this->num_used_scalars++;
      }
    }
  }

  vtkDebugMacro(<<"vtkNek5000Reader::updateVariableStatus: Rank: "<<my_rank<< ": this->num_used_scalars= "<<this->num_used_scalars<<" : this->num_used_vectors= "<<this->num_used_vectors);
}

//----------------------------------------------------------------------------
int vtkNek5000Reader::GetVariableNamesFromData(char* varTags)
{
  int   ind = 0;
  char  l_var_name[2];
  int numSFields=0;

  char* sPtr = nullptr;
  sPtr = strchr(varTags, 'S');
  if(sPtr)
  {
      sPtr++;
      while(*sPtr == ' ')
          sPtr++;
      char digit1 = *sPtr;
      sPtr++;
      while(*sPtr == ' ')
          sPtr++;
      char digit2 = *sPtr;
    
      if (digit1 >= '0' && digit1 <= '9' &&
          digit2 >= '0' && digit2 <= '9')
          numSFields = (digit1-'0')*10 + (digit2-'0');
      else
          numSFields = 1;
  }

  this->num_vars = 0;

  l_var_name[1] = '\0';

  int my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
    {
      my_rank = ctrl->GetLocalProcessId();
    }
    else
    {
      my_rank = 0;
    }

  int len = strlen(varTags);
//  vtkDebugMacro(<< "vtkNek5000Reader::GetVariableNamesFromData:after strlen my_rank: "<<my_rank<< "  varTags = \'"<< varTags<< "\'  len= "<< len);

  // allocate space for variable names and lengths,
  // will be at most 4 + numSFields  (4 for velocity, velocity_magnitude, pressure and temperature)
  this->var_names =  (char**)malloc((4+numSFields) * sizeof(char*));
  for (int i=0; i < 4+numSFields; i++)
    this->var_names[i] = nullptr;

  this->var_length = new int[4+numSFields];

  while(ind<len)
  {
   switch(varTags[ind])
   {
          case 'X':
          case 'Y':
          case 'Z':
              // if it is a coordinate, we have already accounted for that
              ind++;
              break;

          case 'U':
              this->PointDataArraySelection->AddArray("Velocity");
              this->var_names[this->num_vars] = strdup("Velocity");
vtkDebugMacro(<< "GetVariableNamesFromData:  this->var_names[" << this->num_vars << "] = " << this->var_names[this->num_vars]);
              this->var_length[this->num_vars] = 3; // this is a vector
              //this->velocity_index = ind;
              ind++;
              this->num_vars++;
              // Also add a magnitude scalar
              this->PointDataArraySelection->AddArray("Velocity Magnitude");
              this->var_names[this->num_vars] = strdup("Velocity Magnitude");
              vtkDebugMacro(<< "GetVariableNamesFromData:  this->var_names[" << this->num_vars << "] = " << this->var_names[this->num_vars]);
              this->var_length[this->num_vars] = 1; // this is a scalar
              this->num_vars++;
              break;

          case 'P':
          this->PointDataArraySelection->AddArray("Pressure");
          this->var_names[this->num_vars] = strdup("Pressure");
          vtkDebugMacro(<< "GetVariableNamesFromData:  this->var_names[" << this->num_vars << "] = " << this->var_names[this->num_vars]);
          this->var_length[this->num_vars] = 1; // this is a scalar
          ind++;
          this->num_vars++;
          break;

          case 'T':
          this->PointDataArraySelection->AddArray("Temperature");
          this->var_names[this->num_vars] = strdup("Temperature");
          vtkDebugMacro(<< "GetVariableNamesFromData:  this->var_names[" << this->num_vars << "] = " << this->var_names[this->num_vars]);
          this->var_length[this->num_vars] = 1; // this is a scalar
          ind++;
          this->num_vars++;
          break;
  
          case 'S':
          for(int sloop=0; sloop<numSFields; sloop++)
            {
            char sname[4];
            sprintf(sname, "S%02d", sloop+1);
            this->PointDataArraySelection->AddArray(sname);
            this->var_names[this->num_vars] = strdup(sname);
vtkDebugMacro(<< "GetVariableNamesFromData:  this->var_names[" << this->num_vars << "] = " << this->var_names[this->num_vars]);
            this->var_length[this->num_vars] = 1; // this is a scalar
            ind+=3;
            this->num_vars++;
            }
          break;
          default:
          ind++;
          break;
   }
    
  } // while(ind<len)

  //this->DisableAllDerivedVariableArrays();

  return len;
}

//----------------------------------------------------------------------------

void vtkNek5000Reader::readData(char* dfName)
{
  long total_header_size = 136 + (this->numBlocks * 4);
  long read_location;
  long read_size;
  ifstream dfPtr;
  float* dataPtr;
  double *tmpDblPtr;

  int my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    my_rank = 0;
  }

  dfPtr.open(dfName, std::ifstream::binary);
  if(dfPtr.is_open())
  {
    // if this data file includes the mesh, add it to header size
    if(this->timestep_has_mesh[this->ActualTimeStep])
    {
      long offset1;
      offset1 = this->numBlocks;
      offset1 *= this->totalBlockSize;
      if (this->MeshIs3D)
        offset1 *= 3; // account for X, Y and Z
      else
        offset1 *= 2;  // account only for X, Y
      offset1 *= this->precision;
      total_header_size += offset1;
    }
    // currently reading a block at a time, if we need doubles, allocate an array for a block
    if(this->precision == 8)
    {
      tmpDblPtr = new double[this->totalBlockSize * 3];
    }

    // for each variable
    long var_offset;
    long l_blocksize, scalar_offset;
    scalar_offset  = this->numBlocks;
    scalar_offset *= this->totalBlockSize;
    scalar_offset *= this->precision;
    
    for(auto i=0; i < this->num_vars; i++)
    {
      if(i < 2){ // if Velocity or Velocity Magnitude
        var_offset = 0;
        }
      else{
        if (this->MeshIs3D)
          var_offset = (3 + (i-2))*scalar_offset; // counts VxVyVz
        else
          var_offset = (2 + (i-2))*scalar_offset; // counts VxVy
          }
      dataPtr = this->dataArray[i];

      if(dataPtr)
      {
        if(strcmp(this->var_names[i], "Velocity") == 0 && !this->MeshIs3D)
          {
          read_size =   this->totalBlockSize * 2;
          }
        else
          {
          read_size =   this->totalBlockSize * this->var_length[i];
          }
        l_blocksize = read_size * this->precision;

        if(this->precision == 4)
        {
          for(auto j=0; j < this->myNumBlocks; j++)
          {
            read_location = total_header_size + var_offset + long(this->myBlockPositions[j] * l_blocksize );
            dfPtr.seekg(read_location, std::ios_base::beg );
            if (!dfPtr)
              std::cerr << __LINE__ << "block="<< j << ": seekg error for block position = " << this->myBlockPositions[j] << std::endl;
            dfPtr.read( (char *)dataPtr, read_size*sizeof(float));
            if (!dfPtr)
              std::cerr << __LINE__ << ": read error for paylood of " << read_size << " floats = " << read_size*sizeof(float) << std::endl;
/*
when reading vectors, such as Velocity, first come all Vx components, then all Vy, then all Vz.
if reading 2D, it is safer to set the Z component to 0.
*/
            if(strcmp(this->var_names[i], "Velocity") == 0 && !this->MeshIs3D)
              {
              //memset(&dataPtr[read_size], 0, read_size*sizeof(float));    
              }
            dataPtr += this->totalBlockSize * this->var_length[i];
          }
          if(this->swapEndian)
          {
            std::cout << "ByteSwap32()\n";
            ByteSwap32(this->dataArray[i], this->myNumBlocks*this->totalBlockSize* this->var_length[i]);
          }
        }
        else // precision == 8
        {
          for(auto j=0; j<this->myNumBlocks; j++)
          {
            read_location = total_header_size + var_offset + long(this->myBlockPositions[j] * l_blocksize );
            dfPtr.seekg(read_location, std::ios_base::beg );
            if (!dfPtr)
              std::cerr << __LINE__ << ": seekg error at read_location = " << read_location << std::endl;
            dfPtr.read( (char *)tmpDblPtr, read_size*sizeof(double));
            if (!dfPtr)
              std::cerr << __LINE__ << ": read error\n";
            for(auto ind=0; ind<read_size; ind++)
            {
              *dataPtr = (float)tmpDblPtr[ind];
              dataPtr++;
            }
          }
          if(this->swapEndian)
            ByteSwap64(this->dataArray[i], this->myNumBlocks*this->totalBlockSize* this->var_length[i]);
        }

        // if this is velocity, also add the velocity magnitude if and only if it has also been requested
        if(strcmp(this->var_names[i], "Velocity") == 0 and this->GetPointArrayStatus("Velocity Magnitude"))
        {
          float vx, vy, vz;
          int coord_offset = this->totalBlockSize;  // number of values for one coordinate (X or Y or Z)
          for(auto j=0; j<this->myNumBlocks; j++)
          {
            int mag_block_offset = j*this->totalBlockSize;
            int comp_block_offset = mag_block_offset * 3;
            for(auto k=0; k<this->totalBlockSize; k++)
            {
              vx = this->dataArray[i][                              comp_block_offset + k];
              vy = this->dataArray[i][               coord_offset + comp_block_offset + k];
              vz = this->dataArray[i][coord_offset + coord_offset + comp_block_offset + k];
              this->dataArray[i+1][mag_block_offset+k] = std::sqrt((vx*vx) + (vy*vy) + (vz*vz));
            }
          }
          i++;  // skip over the velocity magnitude variable, since we just took care of it
        } // if "Velocity"
      } // only read if valid pointer
    }  // for(i=0; i<this->num_vars; i++)

    if(this->precision == 8)
    {
      delete [] tmpDblPtr;
    }
    dfPtr.close();
  }
  else
  {
    std::cerr << "Error opening datafile : " << dfName << endl;
    exit(1);
  }

#ifdef COMPUTE_MIN_MAX
  for(auto i=0; i<this->num_vars; i++)
  {
    float dmin[3]={VTK_FLOAT_MAX, VTK_FLOAT_MAX, VTK_FLOAT_MAX};
    float dmax[3]={VTK_FLOAT_MIN, VTK_FLOAT_MIN, VTK_FLOAT_MIN};
    
    dataPtr = this->dataArray[i];
    if(dataPtr)
    {
      // Check the data ranges
      for(auto j=0; j<this->myNumBlocks; j++)
      {
        for(int a=0; a<this->totalBlockSize; a++)
        {
          if(i==0)
          {
            for(auto k=0; k<3; k++)
            {
               if(this->dataArray[i][j*this->totalBlockSize*3 + a +(this->totalBlockSize*k)] > dmax[k])
                 dmax[k] = this->dataArray[i][j*this->totalBlockSize*3 + a +(this->totalBlockSize*k)];
               if(this->dataArray[i][j*this->totalBlockSize*3 + a +(this->totalBlockSize*k)] < dmin[k])
                 dmin[k] = this->dataArray[i][j*this->totalBlockSize*3 + a +(this->totalBlockSize*k)];
            }
          }
          else
          {
            if(this->dataArray[i][j*this->totalBlockSize + a] > dmax[0])
              dmax[0] = this->dataArray[i][j*this->totalBlockSize + a];
            if(this->dataArray[i][j*this->totalBlockSize + a] < dmin[0])
              dmin[0] = this->dataArray[i][j*this->totalBlockSize + a];
          }
        }
      }

      vtkDebugMacro(<<"Rank: "<< my_rank<< "  dataArray["<<this->var_names[i]<<"] : ["<<dmin[0]<<", "<<dmax[0]<<"]");
      if(!strcmp(this->var_names[i], "Velocity"))
      {
        vtkDebugMacro(<<"Rank: "<< my_rank<< "  dataArray["<<this->var_names[i]<<"][1] : ["<<dmin[1]<<", "<<dmax[1]<<"]");
        vtkDebugMacro(<<"Rank: "<< my_rank<< "  dataArray["<<this->var_names[i]<<"][2] : ["<<dmin[2]<<", "<<dmax[2]<<"]");
      }
    }
  }  // for all vars
#endif

}// vtkNek5000Reader::readData(char* dfName)

//----------------------------------------------------------------------------
    
void vtkNek5000Reader::partitionAndReadMesh()
{
  char dfName[265];
  ifstream dfPtr;
  int i, j, jj;
  string buf2, tag;
  std::map<int,int> blockMap;

  int my_rank;
  int num_ranks;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    my_rank = ctrl->GetLocalProcessId();
    num_ranks = ctrl->GetNumberOfProcesses();
  }
  else
  {
    my_rank = 0;
    num_ranks = 1;
  }

  sprintf(dfName, this->datafile_format.c_str(), 0, this->datafile_start );
  dfPtr.open(dfName);
    
  if ( (dfPtr.rdstate() & std::ifstream::failbit ) != 0 )
  {
    std::cerr << "Error opening : " << dfName << endl;
    exit(1);
  }

  dfPtr >> tag;
  if (tag != "#std")
  {
    cerr<< "Error reading the header.  Expected it to start with #std " << dfName << endl;
    exit(1);
  }
  dfPtr >> this->precision;
  dfPtr >> this->blockDims[0];
  dfPtr >> this->blockDims[1];
  dfPtr >> this->blockDims[2];
  dfPtr >> buf2;  //blocks per file
  dfPtr >> this->numBlocks;

  this->totalBlockSize =  this->blockDims[0] *  this->blockDims[1] *  this->blockDims[2];
  if(this->blockDims[2] > 1)
    this->MeshIs3D = true;
  else
    this->MeshIs3D = false;
  std::cerr << "3DMesh is " << MeshIs3D << ", totalBlockSize = " << this->blockDims[0] <<"*"<<  this->blockDims[1] <<"*"<<  this->blockDims[2] <<"="<< this->totalBlockSize << std::endl;

  float test;
  dfPtr.seekg( 132, std::ios_base::beg );
  dfPtr.read((char *)(&test), 4);

  // see if we need to swap endian
  if (test > 6.5 && test < 6.6)
    this->swapEndian = false;
  else
  {
    ByteSwap32(&test, 1);
    if (test > 6.5 && test < 6.6)
      this->swapEndian = true;
    else
    {
      std::cerr << "Error reading file, while trying to determine endianness : " << dfName << endl;
      exit(1);
    }
  }

  int *tmpBlocks = new int[numBlocks];
  this->proc_numBlocks = new int[num_ranks];

  // figure out how many blocks (elements) each proc will handle
  int elements_per_proc = this->numBlocks / num_ranks;
  int one_extra_until = this->numBlocks % num_ranks;

  for(i=0; i<num_ranks; i++)
  {
    this->proc_numBlocks[i] = elements_per_proc + (i<one_extra_until ? 1 : 0 );
    std::cerr <<"Proc "<< i << " has "<< this->proc_numBlocks[i]<<" blocks"<<endl;
  }
  this->myNumBlocks = this->proc_numBlocks[my_rank];
  this->myBlockIDs = new int[this->myNumBlocks];
  // read the ids of all of the blocks in the file
  dfPtr.seekg( 136, std::ios_base::beg );
  dfPtr.read( (char *)tmpBlocks, this->numBlocks*sizeof(int) );
  if (this->swapEndian)
    ByteSwap32(tmpBlocks, this->numBlocks);

  // add the block locations to a map, so that we can easily find their position based on their id
  for(i=0; i<this->numBlocks; i++)
  {
    blockMap[tmpBlocks[i]] = i;
  }

  // if there is a .map file, we will use that to partition the blocks
  char* map_filename = strdup(this->GetFileName());
  char* ext = strrchr(map_filename, '.');
  int* all_element_list;
  ext++;
  sprintf(ext, "map");
  ifstream  mptr(map_filename);
  int *map_elements = nullptr;
  if(mptr.is_open())
  {
    vtkDebugMacro(<< "vtkNek5000Reader::partitionAndReadMesh: found mapfile: "<<map_filename);
    int num_map_elements;
    mptr >> num_map_elements >> buf2 >> buf2 >> buf2 >> buf2 >> buf2 >> buf2;
    map_elements = new int[num_map_elements ];
    for(i=0; i<num_map_elements; i++)
    {
      mptr >> map_elements[i] >> buf2 >> buf2 >> buf2 >> buf2 >> buf2 >> buf2 >> buf2 >> buf2;
      map_elements[i]+=1;
    }
    mptr.close();
     
    all_element_list = map_elements;
  }
  // otherwise just use the order in the data file
  else
  {
    vtkDebugMacro(<< "vtkNek5000Reader::partitionAndReadMesh: did not find mapfile: "<<map_filename);
    all_element_list = tmpBlocks;
  }
  free(map_filename);

  int start_index=0;
  for(i=0; i<my_rank; i++)
  {
    start_index += this->proc_numBlocks[i];
  }
  // copy my list of elements
  for(i=0; i<this->myNumBlocks; i++)
  {
    this->myBlockIDs[i] = all_element_list[start_index+i];
  }
  // if they came from the map file, sort them
  if(map_elements != nullptr)
    {
    qsort(this->myBlockIDs, this->myNumBlocks, sizeof(int), compare_ids);
    }

  // now that we have our list of blocks, get their positions in the file (their index)
  this->myBlockPositions = new int[this->myNumBlocks];
  std::cerr << __LINE__ << " newing myBlockPositions" << std::endl;
  for(i=0; i<this->myNumBlocks; i++)
  {
    this->myBlockPositions[i] = blockMap.find(this->myBlockIDs[i])->second;
  }

  // TEMP: checking for duplicates within myBlockPositions
  if(map_elements != nullptr)
  {
    for(i=0; i<this->myNumBlocks-1; i++)
    {
      for(j=i+1; j<this->myNumBlocks; j++)
      {
        if(this->myBlockPositions[i] == this->myBlockPositions[j])
        {
          cerr<<"********my_rank: "<< my_rank<< " : Hey (this->myBlockPositions["<<i<<"] and ["<<j<<"] both == "<< this->myBlockPositions[j]<< endl;
        }
      }
    }
  }

  delete [] tmpBlocks;
  if(map_elements != nullptr)
    delete [] map_elements;

  // now read the coordinates for all of my blocks
  if(nullptr == this->meshCoords)
  {
  std::cerr << __LINE__ << " this->meshCoords = new float[" << this->myNumBlocks << "*" << this->totalBlockSize << "*" << "*3]" << std::endl;
    vtkDebugMacro(<< ": partitionAndReadMesh:  ALLOCATE meshCoords[" << this->myNumBlocks <<"*"<< this->totalBlockSize <<"*" <<3 << "]");
    this->meshCoords = new float[this->myNumBlocks * this->totalBlockSize * 3];
  }

  long total_header_size = 136 + (this->numBlocks * 4);
  long read_location, offset1;

  if(this->precision == 4)
  {
    float* coordPtr = this->meshCoords;
    int read_size;
    if(this->MeshIs3D)
      {
      read_size = this->totalBlockSize * 3;
      for(i=0; i < this->myNumBlocks; i++)
        {
      // header + (index_of_this_block * size_of_a_block * variable_in_block (x,y,z) * precision)
        offset1  = this->myBlockPositions[i];
        offset1 *= this->totalBlockSize;
        offset1 *= 3;
        offset1 *= this->precision;
        read_location = total_header_size + offset1;
        dfPtr.seekg(read_location, std::ios_base::beg );
        if (!dfPtr)
          std::cerr << __LINE__ << ": seekg error at read_location = " << read_location << std::endl;
        dfPtr.read( (char *)coordPtr, read_size*sizeof(float));
        if (!dfPtr)
          std::cerr << __LINE__ << ": read error\n";
        coordPtr += read_size;
       }
      }
    else { // 2D case
      read_size = this->totalBlockSize * 2;
      for(i=0; i < this->myNumBlocks; i++)
        {
        // header + (index_of_this_block * size_of_a_block * variable_in_block (x,y,z) * precision)
        offset1  = this->myBlockPositions[i];
        offset1 *= this->totalBlockSize;
        offset1 *= 2;
        offset1 *= this->precision;
        read_location = total_header_size + offset1;
        dfPtr.seekg(read_location, std::ios_base::beg );
        if (!dfPtr)
          std::cerr << __LINE__ << ": seekg error at read_location = " << read_location << std::endl; 
        dfPtr.read( (char *)coordPtr, read_size*sizeof(float));
      
        if (!dfPtr)
          std::cerr << __LINE__ << ": read error\n";
        // now set the Z component to 0.0
        memset(&coordPtr[read_size], 0, this->totalBlockSize * sizeof(float));
        coordPtr += (this->totalBlockSize * 3);
        }
    }

    if(this->swapEndian)
      ByteSwap32(this->meshCoords, this->myNumBlocks * this->totalBlockSize * 3);
  }
  else // precision == 8
  {
    float* coordPtr = this->meshCoords;
    double *tmpDblPts = new double[this->totalBlockSize * 3];
    int read_size = this->totalBlockSize * 3;
    for(i=0; i<this->myNumBlocks; i++)
    {
      // header + (index_of_this_block * size_of_a_block * variable_in_block (x,y,z) * precision)
      read_location = total_header_size + int64_t(this->myBlockPositions[i] * this->totalBlockSize * 3 * this->precision );
      //fseek(dfPtr, read_location, SEEK_SET);
      //fread(tmpDblPts, sizeof(double), read_size, dfPtr);
      dfPtr.seekg(read_location, std::ios_base::beg );
      if (!dfPtr)
        std::cerr << __LINE__ << ": seekg error at read_location = " << read_location << std::endl;
      dfPtr.read( (char *)tmpDblPts, read_size*sizeof(double));
      for(auto ind=0; ind<read_size; ind++)
      {
        *coordPtr = (float)tmpDblPts[ind];
        coordPtr++;
      }
    }
    if(this->swapEndian)
      ByteSwap64(this->meshCoords, this->myNumBlocks*this->totalBlockSize*3);
    delete [] tmpDblPts;
  }
  delete [] this->myBlockIDs;
  dfPtr.close();
}// void vtkNek5000Reader::partitionAndReadMesh()

//----------------------------------------------------------------------------
int vtkNek5000Reader::RequestInformation(
  vtkInformation* vtkNotUsed(request),
  vtkInformationVector** vtkNotUsed(inputVector),
  vtkInformationVector* outputVector)
{
  double timer_diff;

  string tag;
  char buf[2048];

  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
    {
      num_ranks = ctrl->GetNumberOfProcesses();
      my_rank = ctrl->GetLocalProcessId();
    }
    else
    {
      num_ranks = 1;
      my_rank = 0;
    }

  if(!this->IAM_INITIALLIZED)
    {
    // Might consider having just the master node read the .nek5000 file, and broadcast each line to the other processes ??

    char* filename = this->GetFileName();
    ifstream inPtr(this->GetFileName());
    
    //print the name of the file we're supposed to open
    vtkDebugMacro(<< "vtkNek5000Reader::RequestInformation: FileName: " << this->GetFileName());

     // Process a tag at a time until all lines have been read
    while (inPtr.good())
      {
      inPtr >> tag;
      if (inPtr.eof())
        {
        inPtr.clear();
        break;
        }

      if (tag[0] == '#')
        {
        inPtr.getline(buf, 2048);
        continue;
        }

      if (strcasecmp("nek5000", tag.c_str())==0)
        {
        vtkDebugMacro(<< "vtkNek5000Reader::RequestInformation: format: " << tag.c_str());
        }
      else if (strcasecmp("endian:", tag.c_str())==0)
        {
        //This tag is deprecated.  There's a float written into each binary file
        //from which endianness can be determined.
        string  dummy_endianness;
        inPtr >> dummy_endianness;
        }
      else if (strcasecmp("version:", tag.c_str())==0)
        {
        //This tag is deprecated.  There's a float written into each binary file
        //from which endianness can be determined.
        string  dummy_version;
        inPtr >> dummy_version;
        vtkDebugMacro(<< "vtkNek5000Reader::RequestInformation:  version: " << dummy_version);
        }
      else if (strcasecmp("filetemplate:", tag.c_str())==0)
        {
        inPtr >> this->datafile_format;
        vtkDebugMacro(<< "vtkNek5000Reader::RequestInformation:  this->datafile_format: " << this->datafile_format);
        }
      else if (strcasecmp("firsttimestep:", tag.c_str())==0)
        {
        inPtr >> this->datafile_start;
        vtkDebugMacro(<< "vtkNek5000Reader::RequestInformation:  this->datafile_start: " <<  this->datafile_start);
        }
      else if (strcasecmp("numtimesteps:", tag.c_str())==0)
        {
        inPtr >> this->datafile_num_steps;
        vtkDebugMacro(<< "vtkNek5000Reader::RequestInformation:  this->datafile_num_steps: " <<  this->datafile_num_steps);
        }
    else
      {
      snprintf(buf, 2048, "Error parsing file.  Unknown tag %s", tag.c_str());
      cerr << buf << endl;
      exit(1);
      }
    }// while (inPtr.good())

    inPtr.close();

    int ii;
    if (this->datafile_format[0] != '/')
    {
        for (ii = strlen(filename)-1 ; ii >= 0 ; ii--)
        {
            if (filename[ii] == '/' || filename[ii] == '\\')
            {
                this->datafile_format.insert(0, filename, ii+1);
                break;
            }
        }
    }
    if (ii == -1)
    {
#ifdef _WIN32
        _getcwd(buf, 512);
#else
        getcwd(buf, 512);
#endif
        strcat(buf, "/");
        this->datafile_format.insert(0, buf, strlen(buf));
    }

#ifdef _WIN32
    for (ii = 0 ; ii < fileTemplate.size() ; ii++)
    {
        if (fileTemplate[ii] == '/')
            fileTemplate[ii] = '\\';
    }
#endif

    vtkDebugMacro(<< "vtkNek5000Reader::RequestInformation:  this->datafile_format: " << this->datafile_format);

    this->NumberOfTimeSteps = this->datafile_num_steps;

    // GetAllTimes() now also calls GetVariableNamesFromData()
    vtkNew<vtkTimerLog> timer;
    timer->StartTimer();
    this->GetAllTimesAndVariableNames(outputVector);
    timer->StopTimer();
    timer_diff = timer->GetElapsedTime();

    this->use_variable = new bool[this->num_vars];

    vtkDebugMacro(<<"Rank: "<< my_rank <<" :: GetAllTimeSteps time: "<< timer_diff);

    char dfName[265];

    vtkDebugMacro(<<"Rank: "<< my_rank <<" :: this->datafile_start= " << this->datafile_start);

    sprintf(dfName, this->datafile_format.c_str(), 0, this->datafile_start );
    this->SetDataFileName(dfName);

    vtkInformation *outInfo0 = outputVector->GetInformationObject(0);
    outInfo0->Set(vtkAlgorithm::CAN_HANDLE_PIECE_REQUEST(), 1);

    this->IAM_INITIALLIZED = true;
  }// if(!this->IAM_INITIALLIZED)

  return 1;
} // int vtkNek5000Reader::RequestInformation()


int vtkNek5000Reader::RequestData(
  vtkInformation* request,
  vtkInformationVector** vtkNotUsed( inputVector ),
  vtkInformationVector* outputVector)
{
  double timer_diff;
  double total_timer_diff;
  int i;
  char dfName[256];

  vtkNew<vtkTimerLog> timer;
  vtkNew<vtkTimerLog> total_timer;
  total_timer->StartTimer();
  // the default implimentation is to do what the old pipeline did find what
  // output is requesting the data, and pass that into ExecuteData

  // which output port did the request come from
  int outputPort =
    request->Get(vtkDemandDrivenPipeline::FROM_OUTPUT_PORT());

  vtkDebugMacro(<<"RequestData: ENTER: outputPort = "<< outputPort);

  // if output port is negative then that means this filter is calling the
  // update directly, in that case just assume port 0
  if (outputPort == -1)
  {
    outputPort = 0;
  }

  // get the data object
  vtkInformation *outInfo =
    outputVector->GetInformationObject(0);     //(outputPort);

  vtkInformation *outInfoArray[2];
  outInfoArray[0] = outInfo;
    
  vtkInformation *requesterInfo =
    outputVector->GetInformationObject(outputPort);

  int tsLength =
    requesterInfo->Length(vtkStreamingDemandDrivenPipeline::TIME_STEPS());

  double* steps =
    requesterInfo->Get(vtkStreamingDemandDrivenPipeline::TIME_STEPS());

  vtkDebugMacro(<<"RequestData: tsLength= "<< tsLength);

  // *** Not sure whether we will need this
  // Update the status of the requested variables

  this->updateVariableStatus();

  double l_time_val_0 = 0.0;
  double l_time_val_1 = 0.0;

  // Check if a particular time was requested.
  bool hasTimeValue = false;

  vtkDebugMacro(<< __LINE__ <<" RequestData:");
  // Collect the time step requested
  vtkInformationDoubleKey* timeKey =
    vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP();

  if (outInfoArray[outputPort]->Has(timeKey))
  {
    this->TimeValue = outInfoArray[outputPort]->Get(timeKey);
    hasTimeValue = true;
  }

  if(hasTimeValue)
  {
    vtkDebugMacro(<<"RequestData: this->TimeValue= "<< this->TimeValue);

    //find the timestep with the closest value to the requested time value
    int closestStep=0;
    double minDist=-1;
    for (int cnt=0;cnt<tsLength;cnt++)
    {
      //fprintf(stderr, "RequestData: steps[%d]=%f\n", cnt, steps[cnt]);
      double tdist=(steps[cnt]-this->TimeValue>this->TimeValue-steps[cnt])?steps[cnt]-this->TimeValue:this->TimeValue-steps[cnt];
      if (minDist<0 || tdist<minDist)
      {
        minDist=tdist;
        closestStep=cnt;
      }
    }
    this->ActualTimeStep=closestStep;
  }

  vtkDebugMacro(<<"RequestData: this->ActualTimeStep= "<< this->ActualTimeStep);

  // Force TimeStep into the "known good" range. Although this
  if ( this->ActualTimeStep < this->TimeStepRange[0] )
  {
    this->ActualTimeStep = this->TimeStepRange[0];
  }
  else if ( this->ActualTimeStep > this->TimeStepRange[1] )
  {
    this->ActualTimeStep = this->TimeStepRange[1];
  }

  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    num_ranks = ctrl->GetNumberOfProcesses();
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    num_ranks = 1;
    my_rank = 0;
  }
  vtkDebugMacro(<<"RequestData: ENTER: rank: "<< my_rank << "  outputPort: "
                << outputPort << "  this->ActualTimeStep = "<< this->ActualTimeStep);

  vtkUnstructuredGrid* ugrid = vtkUnstructuredGrid::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));
//  vtkUnstructuredGrid* boundary_ugrid = vtkUnstructuredGrid::SafeDownCast(outInfo1->Get(vtkDataObject::DATA_OBJECT()));

  // Save the time value in the output (ugrid) data information.
  if (steps)
  {
    ugrid->GetInformation()->Set(vtkDataObject::DATA_TIME_STEP(),
                                 steps[this->ActualTimeStep]);
  }

//  int new_rst_val = this->p_rst_start + (this->p_rst_inc* this->ActualTimeStep);
  this->requested_step = this->datafile_start + this->ActualTimeStep;

  //  if the step being displayed is different than the one requested
  //if(this->displayed_step != this->requested_step)
  {
    // get the requested object from the list, if the ugrid in the object is NULL
    // then we have not loaded it yet
    this->curObj = this->myList->getObject(this->requested_step);

    if(this->isObjectMissingData())
    {
      // if the step in memory is different than the step requested
      if(this->requested_step != this->memory_step)
      {
        this->I_HAVE_DATA = false;
      }
    }
  }

  // if I have not yet read the geometry, this should only happen once
  if(this->READ_GEOM_FLAG)
  {
    timer->StartTimer();

    this->partitionAndReadMesh();

    timer->StopTimer();
    timer_diff = timer->GetElapsedTime();

    this->READ_GEOM_FLAG = false;

    vtkDebugMacro(<<"vtkNek5000Reader::RequestData:: Rank: "<<my_rank
                  <<" ::Done calling partitionAndReadMesh: Time: "<< timer_diff);
  } // if(this->READ_GEOM_FLAG)

  if(!this->I_HAVE_DATA)
  {
    // See if we have allocated memory to store the data from disk, if not, allocate it
    if(!this->dataArray)
      {
      this->dataArray = new float*[this->num_vars];
// only allocate data array if the varname has been selected
      for(i=0; i<this->num_vars; i++)
        {
        if(this->use_variable[i])
          {
          this->dataArray[i] = new float[this->myNumBlocks * this->totalBlockSize * this->var_length[i]];
          std::cerr<< "variable " << i << " new float[" <<
                  this->myNumBlocks << " * " <<
                  this->totalBlockSize << " * " <<
                  this->var_length[i] << "]\n";
          }
        else
          {
          this->dataArray[i] = nullptr;
          std::cerr<< "variable " << i << " new float[0] (no allocation)\n";
          }
        }
      }

    // Get the file name for requested time step

    sprintf(dfName, this->datafile_format.c_str(), 0, this->requested_step);
    vtkDebugMacro(<<"vtkNek5000Reader::RequestData: Rank: "<< my_rank<<" Now reading data from file: "<< dfName<<" this->requested_step: "<< this->requested_step);

//    vtkNew<vtkTimerLog> timer;
    timer->StartTimer();
    this->readData(dfName);

    timer->StopTimer();
    timer_diff = timer->GetElapsedTime();

    vtkDebugMacro(<<"vtkNek5000Reader::RequestData:: Rank: "<<my_rank<<" ::Done reading data from file: "<< dfName <<":: Read  time: "<< timer_diff);
    this->curObj->setDataFilename(dfName);

    this->I_HAVE_DATA = true;
    this->memory_step = this->requested_step;

  } // if(!this->I_HAVE_DATA)

//  vtkNew<vtkTimerLog> timer;
  timer->StartTimer();
  this->updateVtuData(ugrid); //, boundary_ugrid); // , outputPort);

  timer->StopTimer();
  timer_diff = timer->GetElapsedTime();
  vtkDebugMacro(<<"vtkNek5000Reader::RequestData: Rank: "<<my_rank<<" :: updateVtuData time: "<<  timer_diff);


  this->SetDataFileName(this->curObj->dataFilename);

  total_timer->StopTimer();
  total_timer_diff = total_timer->GetElapsedTime();

  vtkDebugMacro(<<"vtkNek5000Reader::RequestData: Rank: "<<my_rank<< "  outputPort: " << outputPort <<" EXIT :: Total time: "<< total_timer_diff);
  return 1;
}

void vtkNek5000Reader::updateVtuData(vtkUnstructuredGrid* pv_ugrid)
{
  register int i,j,k,n,e,nelmts;
  int ntot;
  double   *z,*w, ave;
  double timer_diff;

  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    num_ranks = ctrl->GetNumberOfProcesses();
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    num_ranks = 1;
    my_rank = 0;
  }

  // if the grid in the curObj is not NULL, we may have everything we need
  if(this->curObj->ugrid)
  {
    vtkDebugMacro(<< "updateVtuData: my_rank= " << my_rank<<": this->curObj->ugrid != NULL, see if it matches");
    if(this->objectMatchesRequest())
      {
      // copy the ugrid
      pv_ugrid->ShallowCopy(this->curObj->ugrid);

      this->displayed_step = this->requested_step;
      vtkDebugMacro(<<"vtkNek5000Reader::updateVtuData: ugrid same, copy : Rank: "<<my_rank);
      this->SetDataFileName(curObj->dataFilename);

      return;
      }
    else if(this->objectHasExtraData())
      {
      for(int vid=0; vid<this->num_vars; vid++)
        {
        if(!this->GetPointArrayStatus(vid) && this->curObj->vars[vid])
          {
          // Does PV already have this array?  If so, remove it.
          if (pv_ugrid->GetPointData()->GetArray(this->var_names[vid]) != nullptr)
           {
           pv_ugrid->GetPointData()->RemoveArray(this->var_names[vid]);
           }
         // Do I already have this array?  If so, remove it.
         if (this->curObj->ugrid->GetPointData()->GetArray(this->var_names[vid]) != nullptr)
           {
           this->curObj->ugrid->GetPointData()->RemoveArray(this->var_names[vid]);
           }
         this->curObj->vars[vid] = false;
         }
       }

      pv_ugrid->ShallowCopy(this->curObj->ugrid);
      this->displayed_step = this->requested_step;
      //if(!this->USE_MESH_ONLY)
      {
        this->SetDataFileName(curObj->dataFilename);
      }
      return;
    } // else if(this->objectHasExtraData())
  }// if(this->curObj->ugrid)


  // otherwise the grid in the curObj is NULL, and/or the resolution has changed,
  // and/or we need more data than is in curObj, we need to do everything

  int Nvert_total = 0;
  int Nelements_total;
  int vort_index = 0;
  int lambda_index = 0;
  int stress_tensor_index = 0;

  vtkSmartPointer<vtkPoints> points;
    
  Nvert_total = this->myNumBlocks *  this->totalBlockSize;
  if(this->MeshIs3D)
    Nelements_total = this->myNumBlocks * (this->blockDims[0]-1) *  (this->blockDims[1]-1) *  (this->blockDims[2]-1);
  else
    Nelements_total = this->myNumBlocks * (this->blockDims[0]-1) *  (this->blockDims[1]-1);
    
  vtkDebugMacro(<<"updateVtuData: rank = "<<my_rank<<" :Nvert_total= "<<Nvert_total<<", Nelements_total= "<<Nelements_total);

  // if we need to calculate the geometry (first time, or it has changed)
  if (this->CALC_GEOM_FLAG)
    {
    vtkNew<vtkTimerLog> timer;
    timer->StartTimer();
    if(this->UGrid)
      {
      this->UGrid->Delete();
      }
    this->UGrid = vtkUnstructuredGrid::New();
    this->UGrid->Allocate(Nelements_total);

    points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints(Nvert_total);

    std::cout << __LINE__ <<" : updateVtuData : rank = "<<my_rank<<": Nelements_total = "<<Nelements_total<<" Nvert_total = "<< Nvert_total << std::endl;

    copyContinuumPoints(points);

    timer->StopTimer();
    timer_diff = timer->GetElapsedTime();
    vtkDebugMacro(<< "updateVtuData: my_rank= " << my_rank<<": time to copy/convert xyz and uvw: "<< timer_diff);
    } // if (this->CALC_GEOM_FLAG)

  vtkDebugMacro(<< "updateVtuData: my_rank= " << my_rank<<": call copyContinuumData()");

  this->copyContinuumData(pv_ugrid);

  vtkNew<vtkTimerLog> timer;
  timer->StartTimer();
  if (this->CALC_GEOM_FLAG)
    {
    addCellsToContinuumMesh();
    if(this->SpectralElementIds)  // optional. If one wants to extract cells belonging to specific spectral element(s)
      addSpectralElementId(Nelements_total);
    this->UGrid->SetPoints(points);
    }

  timer->StopTimer();
  timer_diff = timer->GetElapsedTime();
  vtkDebugMacro(<< "updateVtuData: my_rank= " << my_rank<<": time of CALC_GEOM (the mesh): "<< timer_diff);

  timer->StartTimer();
  vtkNew<vtkCleanUnstructuredGrid> clean;

  vtkNew<vtkUnstructuredGrid> tmpGrid;
  tmpGrid->ShallowCopy(this->UGrid);
  clean->SetInputData(tmpGrid.GetPointer());

  clean->Update();
  timer->StopTimer();
  timer_diff = timer->GetElapsedTime();
  vtkDebugMacro(<< "updateVtuData: my_rank= " << my_rank<<": time to clean the grid: "<< timer_diff);

  timer->StartTimer();
  pv_ugrid->ShallowCopy(clean->GetOutput());

  vtkDebugMacro(<< "updateVtuData: my_rank= " << my_rank<<":  completed ShallowCopy to pv_ugrid\n");
  if(this->curObj->ugrid)
    {
    this->curObj->ugrid->Delete();
    }
  this->curObj->ugrid = vtkUnstructuredGrid::New();

  this->curObj->ugrid->ShallowCopy(this->UGrid);

  this->displayed_step = this->requested_step;
  
  for(int kk=0; kk<this->num_vars; kk++)
    {
    this->curObj->vars[kk] = this->GetPointArrayStatus(kk);
    }

  this->CALC_GEOM_FLAG=false;
} // vtkNek5000Reader::updateVtuData()

void vtkNek5000Reader::addCellsToContinuumMesh()
{
// Note that point ids are starting at 0, and are local to each processor
// same with cellids. Local and starting at 0 on each MPI task
  vtkIdType pts[8];
  int n = 0;
  
  if (this->MeshIs3D)
    {
    for(auto e = 0; e < this->myNumBlocks; ++e)
    {
      for(auto ii = 0; ii < this->blockDims[0]-1; ++ii)
      {
        for(auto jj = 0; jj < this->blockDims[1]-1; ++jj)
        {
          for(auto kk = 0; kk < this->blockDims[2]-1; ++kk)
          {
            pts[0] = kk*(this->blockDims[1])*(this->blockDims[0]) + jj*(this->blockDims[0]) + ii + n;
            pts[1] = kk*(this->blockDims[1])*(this->blockDims[0]) + jj*(this->blockDims[0]) + ii+1 + n;
            pts[2] = kk*(this->blockDims[1])*(this->blockDims[0]) + (jj+1)*(this->blockDims[0]) + ii+1 + n;
            pts[3] = kk*(this->blockDims[1])*(this->blockDims[0]) + (jj+1)*(this->blockDims[0]) + ii + n;
            pts[4] = (kk+1)*(this->blockDims[1])*(this->blockDims[0]) + jj*(this->blockDims[0]) + ii + n;
            pts[5] = (kk+1)*(this->blockDims[1])*(this->blockDims[0]) + jj*(this->blockDims[0]) + ii+1 + n;
            pts[6] = (kk+1)*(this->blockDims[1])*(this->blockDims[0]) + (jj+1)*(this->blockDims[0]) + ii+1 + n;
            pts[7] = (kk+1)*(this->blockDims[1])*(this->blockDims[0]) + (jj+1)*(this->blockDims[0]) + ii + n;

            this->UGrid->InsertNextCell(VTK_HEXAHEDRON, 8, pts);
          }
        }
      }
      n +=  this->totalBlockSize;
    }
    }
  else // 2D
    {
      for(auto e = 0; e < this->myNumBlocks; ++e)
      {
        for(auto ii = 0; ii < this->blockDims[0]-1; ++ii)
        {
          for(auto jj = 0; jj < this->blockDims[1]-1; ++jj)
          {
            pts[0] = n+jj*(this->blockDims[0]) + ii;
            pts[1] = n+jj*(this->blockDims[0]) + ii+1;
            pts[2] = n+(jj+1)*(this->blockDims[0]) + ii+1;
            pts[3] = n+(jj+1)*(this->blockDims[0]) + ii;
            this->UGrid->InsertNextCell(VTK_QUAD, 4, pts);
          }
        }
      n +=  this->totalBlockSize;
      }
    }
}// addPointsToContinuumMesh()

void vtkNek5000Reader::addSpectralElementId(int nelements)
{
  vtkIdType pts[8];
  vtkTypeUInt32Array *spectral_id = vtkTypeUInt32Array::New();
  spectral_id->SetNumberOfTuples(nelements);
  spectral_id->SetName("spectral element id");
  int n = 0;
  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
    {
    my_rank = ctrl->GetLocalProcessId();
    }
  else
    {
    my_rank = 0;
    }

  int start_index=0;
  for(auto i=0; i<my_rank; i++)
  {
    start_index += this->proc_numBlocks[i];
  }
  
  if (this->MeshIs3D)
    {
    for(auto e = start_index; e < start_index+this->myNumBlocks; ++e)
    {
      for(auto ii = 0; ii < this->blockDims[0]-1; ++ii)
      {
        for(auto jj = 0; jj < this->blockDims[1]-1; ++jj)
        {
          for(auto kk = 0; kk < this->blockDims[2]-1; ++kk)
          {
            spectral_id->SetTuple1(n++, e);
          }
        }
      }
    }
    }
  else // 2D
    {
      for(auto e = start_index; e < start_index+this->myNumBlocks; ++e)
      {
        for(auto ii = 0; ii < this->blockDims[0]-1; ++ii)
        {
          for(auto jj = 0; jj < this->blockDims[1]-1; ++jj)
          {
            spectral_id->SetTuple1(n++, e);
          }
        }
      }
    }
    this->UGrid->GetCellData()->AddArray(spectral_id);
    spectral_id->Delete();
}// addSpectralElementId()

void vtkNek5000Reader::copyContinuumPoints(vtkPoints* points)
{
  int index = 0;
  std::cerr<< "begin copyContinuumPoints()\n";
  // for each element/block in the continuum mesh
  for(auto k = 0; k < this->myNumBlocks; ++k)
  {
    int block_offset = k * this->totalBlockSize * 3;  // 3 is for X,Y,Z coordinate components
    // for every point in this element/block
    for(auto i = 0; i < this->totalBlockSize; ++i)
    {/*
      std::cerr<< index << ": " <<
                  this->meshCoords[block_offset+i] << ", " <<
                  this->meshCoords[block_offset+this->totalBlockSize+i] << ", " <<
                  this->meshCoords[block_offset+this->totalBlockSize+this->totalBlockSize+i] << std::endl;
                  */
    points->InsertPoint(index,
                  this->meshCoords[block_offset+i],  // X val
                  this->meshCoords[block_offset+this->totalBlockSize+i],  // Y val
                  this->meshCoords[block_offset+this->totalBlockSize+this->totalBlockSize+i]); // Z val
      index ++;
    }
  }
  delete [] this->meshCoords;
  std::cerr<< "end  copyContinuumPoints()\n";
}

void vtkNek5000Reader::copyContinuumData(vtkUnstructuredGrid* pv_ugrid)
{
  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
    {
    num_ranks = ctrl->GetNumberOfProcesses();
    my_rank = ctrl->GetLocalProcessId();
    }
  else
    {
    num_ranks = 1;
    my_rank = 0;
    }

  int index = 0;
  int cur_scalar_index=0;
  int cur_vector_index=0;
  int num_verts = this->myNumBlocks * this->totalBlockSize;

  vtkFloatArray** scalars;
  scalars = (vtkFloatArray**) malloc(this->num_used_scalars * sizeof(vtkFloatArray*));

  vtkFloatArray** vectors;
  vectors = (vtkFloatArray**) malloc(this->num_used_vectors * sizeof(vtkFloatArray*));

  // allocate arrays for used scalars and vectors
  for(auto jj=0; jj < this->num_vars; jj++)
  {
    if(this->GetPointArrayStatus(jj) == 1)
    {
      std::cerr<< "copying [" << this->var_names[jj] << "] of size " << this->var_length[jj] << "\n";
      // if this variable is a scalar
      if(this->var_length[jj] == 1)
      {
        vtkDebugMacro(<< "copyContinuumData: my_rank= " << my_rank<<": var["<<jj<<"]: allocate scalars["<<cur_scalar_index<<"]:  name= "<< this->var_names[jj]);
        scalars[cur_scalar_index] = vtkFloatArray::New();
        scalars[cur_scalar_index]->SetNumberOfComponents(1);
        scalars[cur_scalar_index]->SetNumberOfValues(num_verts);
        scalars[cur_scalar_index]->SetName(this->var_names[jj]);
        cur_scalar_index++;
      }
      // if this variable is a vector
      else if(this->var_length[jj] > 1)
      {
        vtkDebugMacro(<< "copyContinuumData: my_rank= " << my_rank<<": var["<<jj<<"]: allocate vectors["<<cur_vector_index<<"]:  name= "<< this->var_names[jj]);
        vectors[cur_vector_index] = vtkFloatArray::New();
        vectors[cur_vector_index]->SetNumberOfComponents(3);
        vectors[cur_vector_index]->SetNumberOfTuples(num_verts);
        vectors[cur_vector_index]->SetName(this->var_names[jj]);
        cur_vector_index++;
      }
    }
  }

  cur_scalar_index=0;
  cur_vector_index=0;

  // for each variable
  for(auto v_index=0; v_index < this->num_vars; v_index++)
  {
    if(this->use_variable[v_index])
    {
      // if this is a scalar
      if(this->var_length[v_index] == 1)
      {
        index=0;
        // for each  element/block in the continuum mesh
        for(int b_index = 0; b_index < this->myNumBlocks; ++b_index)
        {
          // for every point in this element/block
          for(int p_index = 0; p_index < this->totalBlockSize; ++p_index)
          {
             scalars[cur_scalar_index]->SetValue(index, this->dataArray[v_index][index]);
             index++;
          }
        }
    
        this->UGrid->GetPointData()->AddArray(scalars[cur_scalar_index]);
        scalars[cur_scalar_index]->Delete();
        cur_scalar_index++;
      }
      // if this is a vector
      else if(this->var_length[v_index] > 1)
      {
        index=0;
        // for each  element/block in the continuum mesh
        for(int b_index = 0; b_index < this->myNumBlocks; ++b_index)
        {
          // for every point in this element/block
          // cerr<<"rank= "<<my_rank<<" : b_index= "<< b_index<<endl;
          int mag_block_offset = b_index*this->totalBlockSize;
          int comp_block_offset = mag_block_offset * 3;

          for(int p_index = 0; p_index < this->totalBlockSize; ++p_index)
          {
            float vx, vy, vz;
            vx = this->dataArray[v_index][comp_block_offset + p_index];
            vy = this->dataArray[v_index][comp_block_offset + p_index + this->totalBlockSize];
            vz = this->dataArray[v_index][comp_block_offset + p_index + this->totalBlockSize + this->totalBlockSize];
            vectors[cur_vector_index]->SetTuple3(index, vx, vy, vz);
            index++;
          }
        }

        this->UGrid->GetPointData()->AddArray(vectors[cur_vector_index]);
        vectors[cur_vector_index]->Delete();
        cur_vector_index++;
      }
    }// if(this->use_variable[v_index])
    else
    {
      // remove array if present, it is not needed
      if (pv_ugrid->GetPointData()->GetArray(this->var_names[v_index]) != NULL)
      {
        pv_ugrid->GetPointData()->RemoveArray(this->var_names[v_index]);
      }
      // Do I already have this array?  If so, remove it.
      if (this->UGrid->GetPointData()->GetArray(this->var_names[v_index]) != NULL)
      {
        this->UGrid->GetPointData()->RemoveArray(this->var_names[v_index]);
      }
    }
  }

  free(scalars);
  free(vectors);
} // vtkNek5000Reader::copyContinuumData()

// see if the current object is missing data that was requested
// return true if it is, otherwise false
bool vtkNek5000Reader::isObjectMissingData()
{
  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    num_ranks = ctrl->GetNumberOfProcesses();
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    num_ranks = 1;
    my_rank = 0;
  }
    
// check the stored variables
  for(int i=0; i<this->num_vars; i++)
  {
    if(this->GetPointArrayStatus(i) ==1 && !this->curObj->vars[i])
    {
      return(true);
    }
  }

   return(false);
}// vtkNek5000Reader::isObjectMissingData()

bool vtkNek5000Reader::objectMatchesRequest()
{
// see if the current object matches the requested data
// return false if it does not match, otherwise true    
  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    num_ranks = ctrl->GetNumberOfProcesses();
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    num_ranks = 1;
    my_rank = 0;
  }

  for(int i=0; i<this->num_vars; i++)
  {
    if(this->GetPointArrayStatus(i) != this->curObj->vars[i])
    {
      return(false);
    }
  }
  return(true);  
}// vtkNek5000Reader::objectMatchesRequest()


bool vtkNek5000Reader::objectHasExtraData()
{
// see if the current object has extra data than was requested
// return false if object has less than request, otherwise true
    
  int num_ranks, my_rank;
  vtkMultiProcessController* ctrl = vtkMultiProcessController::GetGlobalController();
  if (ctrl != nullptr)
  {
    num_ranks = ctrl->GetNumberOfProcesses();
    my_rank = ctrl->GetLocalProcessId();
  }
  else
  {
    num_ranks = 1;
    my_rank = 0;
  }

// check the stored variables, if it was requested, but it is not in the current object, return false
  for(int i=0; i<this->num_vars; i++)
  {
    if(this->GetPointArrayStatus(i) && !this->curObj->vars[i])
    {
      return(false);
    }
 }
   
  vtkDebugMacro(<< "objectHasExtraData(): my_rank= " << my_rank<<" : returning true");
  return(true);    
}// vtkNek5000Reader::objectHasExtraData()

int vtkNek5000Reader::CanReadFile(const char* fname)
{
  FILE* fp;
  if ((fp = vtksys::SystemTools::Fopen(fname, "r")) == nullptr)
  {
    return 0;
  }
  else
    return 1;
}// vtkNek5000Reader::CanReadFile()

nek5KObject::nek5KObject()
{
  this->ugrid = NULL;
  this->vorticity = false;
  this->lambda_2 = false;

  for(int ii=0; ii<MAX_VARS; ii++)
  {
    this->vars[ii] = false;
  }

  this->index = 0;
  this->prev = nullptr;
  this->next = nullptr;
  this->dataFilename = nullptr;
}

nek5KObject::~nek5KObject()
{
  if(this->ugrid)
    this->ugrid->Delete();
  if(this->dataFilename)
  {
    free(this->dataFilename);
    this->dataFilename = nullptr;
  }
}

void nek5KObject::reset()
{
  this->vorticity = false;
  this->lambda_2 = false;

  for(int ii=0; ii<MAX_VARS; ii++)
  {
    this->vars[ii] = false;
  }
  this->index = 0;

  if(this->ugrid)
  {
    this->ugrid->Delete();
    this->ugrid = nullptr;
  }

  if(this->dataFilename)
  {
    free(this->dataFilename);
    this->dataFilename = nullptr;
  }
}

void nek5KObject::setDataFilename(char* filename)
{
  if(this->dataFilename)
  {
    free(this->dataFilename);
  }
  this->dataFilename = strdup(filename);
}

nek5KList::nek5KList()
{
  this->head = nullptr;
  this->tail = nullptr;
  this->max_count = 10;
  this->cur_count = 0;
}

nek5KList::~nek5KList()
{
  int new_cnt = 0;
  nek5KObject* curObj = this->head;
  while (curObj && new_cnt<this->cur_count)
  {
    this->head = this->head->next;
    delete curObj;
    curObj = this->head;
    new_cnt++;
  }
}

nek5KObject* nek5KList::getObject(int id)
{
    nek5KObject* curObj = this->head;
    while (curObj)
      {
      if (curObj->index == id)  // if we found it
        {
        // move found obj to tail of the list
        // if already tail, do nothing
        if(curObj == this->tail)
        break;
    
        // if it's the head, update head to next
        if(curObj == this->head)
          {
          this->head = this->head->next;
          }
        // now move curObj to tail
        curObj->next->prev = curObj->prev;
        if(curObj->prev) // i.e. if current was not the head
          {
          curObj->prev->next = curObj->next;
          }
        this->tail->next = curObj;
        curObj->prev = this->tail;
        curObj->next = nullptr;
        this->tail = curObj;
        break;
        }
      else // otherwise, lok at the next one
        {
        curObj = curObj->next;
        }
      }

    // if we didn't find it
    if(curObj == nullptr)
      {
      // if we are not over allocated,
      // create a new object, and put it at the tail
      if(this->cur_count < this->max_count)
        {
        this->cur_count++;
        //curObj = nek5KObject::New();
        curObj = new nek5KObject();
        if (this->head == nullptr)  // if list is empty
          {
          this->head = curObj;
          this->tail = curObj;
          }
        else
          {
          this->tail->next = curObj;
          curObj->prev = this->tail;
          curObj->next = nullptr;
          this->tail = curObj;
          }
        // set the index to the one requested
        curObj->index = id;
        }
      else  // otherwise reuse oldest obj (head), reset and move to tail
        {
        curObj = this->head;
        this->head = this->head->next;
        this->head->prev = nullptr;

        this->tail->next = curObj;
        curObj->prev = this->tail;
        curObj->next = nullptr;
        this->tail = curObj;
        curObj->reset();
        curObj->index = id;
        }
      }
    return(curObj);
}

void ByteSwap32(void *aVals, int nVals)
{
  char *v = (char *)aVals;
  char tmp;
  for (long ii = 0 ; ii < nVals ; ii++, v+=4)
  {
    tmp = v[0]; v[0] = v[3]; v[3] = tmp;
    tmp = v[1]; v[1] = v[2]; v[2] = tmp;
  }
}

void ByteSwap64(void *aVals, int nVals)
{
  char *v = (char *)aVals;
  char tmp;
  for (long ii = 0 ; ii < nVals ; ii++, v+=8)
    {
    tmp = v[0]; v[0] = v[7]; v[7] = tmp;
    tmp = v[1]; v[1] = v[6]; v[6] = tmp;
    tmp = v[2]; v[2] = v[5]; v[5] = tmp;
    tmp = v[3]; v[3] = v[4]; v[4] = tmp;
    }
}

int compare_ids(const void *id1, const void *id2)
{
  int *a = (int*)id1;
  int *b = (int*)id2;
    
  if(*a<*b)
    return(-1);
  if(*a>*b)
    return(1);
  return(0);
}
