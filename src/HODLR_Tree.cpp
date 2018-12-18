#include "HODLR_Tree.hpp"

HODLR_Tree::HODLR_Tree(int n_levels, double tolerance, HODLR_Matrix* A) 
{
    this->n_levels  = n_levels;
    this->tolerance = tolerance;
    this->A         = A;
    this->N         = A->N;
    nodes_in_level.push_back(1);

    for (int j = 1; j <= n_levels; j++) 
    {
        nodes_in_level.push_back(2 * nodes_in_level.back());
    }
    
    this->createTree();
}

// Destructor:
HODLR_Tree::~HODLR_Tree() 
{
    for(int j = 0; j <= n_levels; j++) 
    {
        #pragma omp parallel for
        for(int k = 0; k < nodes_in_level[j]; k++) 
        {
            delete tree[j][k];
        }
    }
}

void HODLR_Tree::createRoot() 
{
    HODLR_Node* root = new HODLR_Node(0, 0, 0, 0, N, tolerance);
    std::vector<HODLR_Node*> level;
    level.push_back(root);
    tree.push_back(level);
}

void HODLR_Tree::createChildren(int level_number, int node_number) 
{
    //  Adding left child:
    HODLR_Node* left = new HODLR_Node(level_number + 1, 2 * node_number, 0, 
                                      tree[level_number][node_number]->c_start[0], 
                                      tree[level_number][node_number]->c_size[0], 
                                      tolerance
                                     );
    tree[level_number + 1].push_back(left);

    //  Adding right child
    HODLR_Node* right = new HODLR_Node(level_number + 1, 2 * node_number + 1, 1, 
                                       tree[level_number][node_number]->c_start[1], 
                                       tree[level_number][node_number]->c_size[1], 
                                       tolerance
                                      );
    tree[level_number + 1].push_back(right);
}

void HODLR_Tree::createTree() 
{
    this->createRoot();
    
    for(int j = 0; j < n_levels; j++) 
    {
        std::vector<HODLR_Node*> level;
        tree.push_back(level);
        
        for(int k = 0; k < nodes_in_level[j]; k++) 
        {
            this->createChildren(j, k);
        }
    }
}

void HODLR_Tree::assembleTree(bool is_sym) 
{
    this->is_sym = is_sym;
    // Assembly of nonleaf nodes:
    for(int j = 0; j < n_levels; j++) 
    {
        #pragma omp parallel for
        for(int k = 0; k < nodes_in_level[j]; k++) 
        {
            tree[j][k]->assembleNonLeafNode(A, is_sym);
        }
    }

    // Assembly of leaf nodes:
    #pragma omp parallel for
    for (int k = 0; k < nodes_in_level[n_levels]; k++) 
    {
        tree[n_levels][k]->assembleLeafNode(A);
    }
}

void HODLR_Tree::printNodeDetails(int level_number, int node_number)
{
    tree[level_number][node_number]->printNodeDetails();
}

void HODLR_Tree::printTreeDetails()
{
    for(int j = 0; j <= n_levels; j++)
    {
        for(int k = 0; k < nodes_in_level[j]; k++)
        {
            this->printNodeDetails(j, k);
            cout << "=======================================================================================================================================" << endl;
        }
        cout << endl << endl;
    }
}

void HODLR_Tree::matmatProduct(MatrixXd x, MatrixXd& b) 
{
    // Initializing matrix b:
    b = MatrixXd::Zero(N, x.cols());

    // At non-leaf levels:
    for (int j = 0; j < n_levels; j++) 
    {
        #pragma omp parallel for
        for (int k = 0; k < nodes_in_level[j]; k++) 
        {
            tree[j][k]->matmatProductNonLeaf(x, b, is_sym);
        }
    }

    // At leaf level:
    #pragma omp parallel for
    for(int k = 0; k < nodes_in_level[n_levels]; k++) 
    {
        tree[n_levels][k]->matmatProductLeaf(x, b);
    }
}

void HODLR_Tree::factorizeLeaf(int k) 
{
    int child;
    int parent = k;
    int size   = tree[n_levels][k]->n_size;
    
    int t_start, r;
    
    if(is_sym == true)
    {
        tree[n_levels][k]->K_factor_LLT.compute(tree[n_levels][k]->K);
        
        #pragma omp parallel for
        for(int l = n_levels - 1; l >= 0; l--) 
        {
            child   = parent % 2;
            parent  = parent / 2;
            t_start = tree[n_levels][k]->n_start - tree[l][parent]->c_start[child];
            r       = tree[l][parent]->rank[child]; // NOTE: Here the rank for both childs is the same

            tree[l][parent]->Q_factor[child].block(t_start, 0, size, r) =   
            this->solveLeaf(k, tree[l][parent]->Q_factor[child].block(t_start, 0, size, r));
        }
    }

    else
    {
        tree[n_levels][k]->K_factor_LU.compute(tree[n_levels][k]->K);

        #pragma omp parallel for
        for(int l = n_levels - 1; l >= 0; l--) 
        {
            child   = parent % 2;
            parent  = parent / 2;
            t_start = tree[n_levels][k]->n_start - tree[l][parent]->c_start[child];
            r       = tree[l][parent]->rank[child];

            tree[l][parent]->U_factor[child].block(t_start, 0, size, r) =   
            this->solveLeaf(k, tree[l][parent]->U_factor[child].block(t_start, 0, size, r));
        }
    }
}

void HODLR_Tree::qr(int j, int k)
{
    int r0 = std::min(tree[j][k]->Q_factor[0].rows(), 
                      tree[j][k]->Q_factor[0].cols()
                     );
    
    int r1 = std::min(tree[j][k]->Q_factor[1].rows(),
                      tree[j][k]->Q_factor[1].cols()
                     );

    Eigen::HouseholderQR<Eigen::MatrixXd> qr(tree[j][k]->Q_factor[0]);

    tree[j][k]->Q_factor[0] = qr.householderQ() * 
                              Eigen::MatrixXd::Identity(tree[j][k]->Q_factor[0].rows(), r0);
    tree[j][k]->K          *= qr.matrixQR().block(0, 0, r0, 
                                                  tree[j][k]->Q_factor[0].cols()
                                                 ).triangularView<Eigen::Upper>();

    qr = Eigen::HouseholderQR<Eigen::MatrixXd>(tree[j][k]->Q_factor[1]);
    tree[j][k]->Q_factor[1] = qr.householderQ() * 
                              Eigen::MatrixXd::Identity(tree[j][k]->Q_factor[1].rows(), r1);
    tree[j][k]->K          *= qr.matrixQR().block(0, 0, r1, 
                                                  tree[j][k]->Q_factor[1].cols()
                                                 ).triangularView<Eigen::Upper>().transpose();
}

// Obtains the QR decomposition of all nodes at this level:
void HODLR_Tree::qrForLevel(int level)
{
    #pragma omp parallel for
    for(int k = 0; k < nodes_in_level[level]; k++)
    {
        qr(level, k);
    }
}

void HODLR_Tree::factorizeNonLeaf(int j, int k) 
{
    int r0 = tree[j][k]->rank[0];
    int r1 = tree[j][k]->rank[1];

    if(is_sym == true)
    {
        tree[j][k]->K_factor_LLT.compute(  MatrixXd::Identity(r0, r1) 
                                         - tree[j][k]->K.transpose() * tree[j][k]->K
                                        );

        int parent = k;
        int child  = k;
        int size   = tree[j][k]->n_size;
        int t_start, r;

        for(int l = j - 1; l >= 0; l--) 
        {
            child   = parent % 2;
            parent  = parent / 2;
            t_start = tree[j][k]->n_start - tree[l][parent]->c_start[child];
            r       = tree[l][parent]->rank[child];

            if(tree[l][parent]->Q_factor[child].cols() > 0)
            {
                tree[l][parent]->Q_factor[child].block(t_start, 0, size, r) =   
                this->solveNonLeaf(j, k, tree[l][parent]->Q_factor[child].block(t_start, 0, size, r));
            }
        }
    }

    else
    {
        if(r0 > 0 || r1 > 0)
        {
            tree[j][k]->K.block(0, r0, r0, r1)  =   
            tree[j][k]->V_factor[1].transpose() * tree[j][k]->U_factor[1];

            tree[j][k]->K.block(r0, 0, r1, r0)  =   
            tree[j][k]->V_factor[0].transpose() * tree[j][k]->U_factor[0];

            tree[j][k]->K_factor_LU.compute(tree[j][k]->K);

            int parent = k;
            int child  = k;
            int size   = tree[j][k]->n_size;
            int t_start, r;

            for(int l = j - 1; l >= 0; l--) 
            {
                child   = parent % 2;
                parent  = parent / 2;
                t_start = tree[j][k]->n_start - tree[l][parent]->c_start[child];
                r       = tree[l][parent]->rank[child];

                if(tree[l][parent]->U_factor[child].cols() > 0)
                {
                    tree[l][parent]->U_factor[child].block(t_start, 0, size, r) =   
                    this->solveNonLeaf(j, k, tree[l][parent]->U_factor[child].block(t_start, 0, size, r));
                }
            }
        }
    }
}

void HODLR_Tree::factorize() 
{
    // Initializing for the non-leaf levels:
    for(int j = 0; j < n_levels; j++) 
    {
        // #pragma omp parallel for
        for(int k = 0; k < nodes_in_level[j]; k++) 
        {
            int &r0 = tree[j][k]->rank[0];
            int &r1 = tree[j][k]->rank[1];

            // Initializing the factorized matrices for the left and right child:
            if(is_sym == true)
            {
                tree[j][k]->Q_factor[0] = tree[j][k]->Q[0];
                tree[j][k]->Q_factor[1] = tree[j][k]->Q[1];
                tree[j][k]->K           = MatrixXd::Identity(r0, r1); // NOTE: r0 == r1
            }

            // Initializing the factorized matrices for the left and right child:
            else
            {
                tree[j][k]->U_factor[0] = tree[j][k]->U[0];
                tree[j][k]->U_factor[1] = tree[j][k]->U[1];
                tree[j][k]->V_factor[0] = tree[j][k]->V[0];
                tree[j][k]->V_factor[1] = tree[j][k]->V[1];
                tree[j][k]->K           = MatrixXd::Identity(r0 + r1, r0 + r1);
            }
        }
    }

    // #pragma omp parallel for
    for(int k = 0; k < nodes_in_level[n_levels]; k++) 
    {
        this->factorizeLeaf(k);
    }

    if(is_sym == true)
    {
        qrForLevel(n_levels - 1);
    }

    // Factorizing the nonleaf levels:
    for(int j = n_levels - 1; j > 0; j--) 
    {
        #pragma omp parallel for
        for(int k = 0; k < nodes_in_level[j]; k++) 
        {
            this->factorizeNonLeaf(j, k);
        }

        if(is_sym == true)
        {
            qrForLevel(j-1);
        }
    }

    if(is_sym == true)
    {
        if(n_levels > 0)
        {
            tree[0][0]->K_factor_LLT.compute(  MatrixXd::Identity(tree[0][0]->rank[0], tree[0][0]->rank[1]) 
                                             - tree[0][0]->K.transpose() * tree[0][0]->K
                                            );
        }
    }
}

// Solve at the leaf is just directly performed:
MatrixXd HODLR_Tree::solveLeaf(int k, MatrixXd b) 
{
    MatrixXd x;

    if(is_sym == true)
    {
        x = tree[n_levels][k]->K_factor_LLT.solve(b);
    }

    else
    {
        x = tree[n_levels][k]->K_factor_LU.solve(b);
    }

    return x;
}

MatrixXd HODLR_Tree::solveNonLeaf(int j, int k, MatrixXd b) 
{
    if(is_sym == true)
    {
        int n0 = tree[j][k]->Q[0].rows();
        int n1 = tree[j][k]->Q[1].rows();
        int r  = b.cols();

        MatrixXd tmp = tree[j][k]->Q_factor[1].transpose() * b.block(n0, 0, n1, r);
        // TODO:Not clear about this yet
        b.block(n0, 0, n1, r) -=   tree[j][k]->Q_factor[1]
                                     * (  tree[j][k]->K_factor_LLT.matrixL().solve((tree[j][k]->K.transpose()
                                        *(tree[j][k]->Q_factor[0].transpose() * b.block(0, 0, n0, r))) - tmp) + tmp
                                       );
        return(b);
    }

    else
    {
        int r0 = tree[j][k]->rank[0];
        int r1 = tree[j][k]->rank[1];
        int n0 = tree[j][k]->c_size[0];
        int n1 = tree[j][k]->c_size[1];
        int r  = b.cols();

        // Initializing the temp matrix that is then factorized:
        MatrixXd temp(r0 + r1, r);
        temp << tree[j][k]->V_factor[1].transpose() * b.block(n0, 0, n1, r),
                tree[j][k]->V_factor[0].transpose() * b.block(0,  0, n0, r);
        temp = tree[j][k]->K_factor_LU.solve(temp);
        
        MatrixXd y(n0 + n1, r);
        y << tree[j][k]->U_factor[0] * temp.block(0,  0, r0, r), 
             tree[j][k]->U_factor[1] * temp.block(r0, 0, r1, r);
        
        return(b - y);
    }
}

MatrixXd HODLR_Tree::solve(MatrixXd b) 
{
    int start, size;
    MatrixXd x = MatrixXd::Zero(b.rows(),b.cols());
    
    int r = b.cols();

    for(int k = 0; k < nodes_in_level[n_levels]; k++) 
    {
        start = tree[n_levels][k]->n_start;
        size  = tree[n_levels][k]->n_size;

        x.block(start, 0, size, r) = this->solveLeaf(k, b.block(start, 0, size, r));
    }

    b = x;
    
    for(int j = n_levels - 1; j >= 0; j--) 
    {
        for (int k = 0; k < nodes_in_level[j]; k++) 
        {
            start = tree[j][k]->n_start;
            size  = tree[j][k]->n_size;
            
            x.block(start, 0, size, r) = this->solveNonLeaf(j, k, b.block(start, 0, size, r));
        }

        b = x;
    }

    return x;
} 

double HODLR_Tree::logDeterminant()
{
    double log_det = 0.0;

    if(is_sym == true)
    {
        for(int j = n_levels; j >= 0; j--) 
        {
            for(int k = 0; k < nodes_in_level[j]; k++) 
            {
                if(tree[j][k]->K.size() > 0)
                {
                    for(int l = 0; l < tree[j][k]->K_factor_LLT.matrixL().rows(); l++) 
                    {   
                        log_det += log(fabs(tree[j][k]->K_factor_LLT.matrixL()(l,l)));
                    }
                }
            }
        }

        log_det *= 2;
    }

    else
    {
        for(int j = n_levels; j >= 0; j--) 
        {
            for(int k = 0; k < nodes_in_level[j]; k++) 
            {
                if(tree[j][k]->K.size() > 0)
                {
                    for(int l = 0; l < tree[j][k]->K_factor_LU.matrixLU().rows(); l++) 
                    {   
                        log_det += log(fabs(tree[j][k]->K_factor_LU.matrixLU()(l,l)));
                    }
                }
            }
        }
    }

    return(log_det);
}

void HODLR_Tree::plotTree()
{
    std::string HODLR_PATH = std::getenv("HODLR_PATH");
    std::string FILE       = HODLR_PATH + "/src/plot_tree.py";
    std::string COMMAND    = "python " + FILE;
    std::ofstream myfile;
    myfile.open ("rank.txt");
    // First entry is the number of levels:
    myfile << n_levels << endl;
    for(int j = 0; j < n_levels; j++)
    {
        for(int k = 0; k < nodes_in_level[j]; k++)
        {
            myfile << tree[j][k]->rank[0] << endl;
        }
    }
    // Closing the file:
    myfile.close();
    cout << "Plotting Tree..." << endl;
    system(COMMAND.c_str());
}